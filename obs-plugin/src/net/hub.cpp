// SPDX-License-Identifier: GPL-2.0-or-later
#include "hub.hpp"

#include "../source/phone-source.hpp"
#include "plugin-support.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QFile>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

using nlohmann::json;

namespace rac {

static int64_t nowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

static int64_t nowUs()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

Hub &Hub::get()
{
	static Hub hub;
	return hub;
}

// ---- identity: the secret, the PC id and the port, kept across restarts -------------------

static std::string configFile(const char *name)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path(name);
	std::string s = p ? p : name;
	bfree(p);
	return s;
}

static std::string randomString(const char *alphabet, int n)
{
	std::string s;
	const int len = int(strlen(alphabet));
	for (int i = 0; i < n; i++)
		s += alphabet[QRandomGenerator::system()->bounded(len)];
	return s;
}

void Hub::loadIdentity()
{
	QFile f(QString::fromStdString(configFile("identity.json")));
	if (f.open(QIODevice::ReadOnly)) {
		json j = json::parse(f.readAll().toStdString(), nullptr, false);
		if (j.is_object()) {
			secret_ = j.value("secret", "");
			pcId_ = j.value("pcId", "");
			port_ = uint16_t(j.value("port", int(kDefaultPort)));
		}
	}
	if (secret_.size() < 16 || pcId_.empty()) {
		// 22 base64url characters: 132 bits, as PROTOCOL.md asks.
		secret_ = randomString("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", 22);
		pcId_ = randomString("0123456789abcdef", 12);
		saveIdentity();
	}
}

void Hub::saveIdentity()
{
	QFile f(QString::fromStdString(configFile("identity.json")));
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		json j = {{"secret", secret_}, {"pcId", pcId_}, {"port", port_}};
		f.write(QByteArray::fromStdString(j.dump(1)));
	}
}

std::string Hub::pcName() const
{
	return QSysInfo::machineHostName().toStdString();
}

std::string Hub::apkPath() const
{
	char *p = obs_module_file("RelyACam.apk");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

// ---- start and stop ---------------------------------------------------------------------

void Hub::start()
{
	if (thread_)
		return;
	loadIdentity();
	thread_ = new QThread();
	thread_->setObjectName("relyacam-net");
	moveToThread(thread_);
	connect(thread_, &QThread::started, this, [this] { listen(); });
	thread_->start();
}

void Hub::stop()
{
	if (!thread_)
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			std::vector<PhoneLink *> all;
			{
				std::lock_guard<std::mutex> l(m_);
				for (auto &kv : links_)
					all.push_back(kv.second);
			}
			// BYE tells each phone to release its camera and mic now (the must-never list).
			for (auto *link : all)
				link->close(true);
			delete server_;
			delete udp_;
			server_ = nullptr;
			udp_ = nullptr;
			moveToThread(QCoreApplication::instance()->thread());
		},
		Qt::BlockingQueuedConnection);
	thread_->quit();
	thread_->wait();
	delete thread_;
	thread_ = nullptr;
}

void Hub::listen()
{
	server_ = new QTcpServer(this);
	bool ok = false;
	for (int i = 0; i < 20 && !ok; i++) {
		if (server_->listen(QHostAddress::AnyIPv4, quint16(port_ + i))) {
			ok = true;
			if (i) {
				port_ = uint16_t(port_ + i);
				saveIdentity();
			}
		}
	}
	if (!ok) {
		obs_log(LOG_ERROR, "could not listen on ports %d-%d", port_, port_ + 19);
		emit message(QStringLiteral("Rely-A-Cam can't open a network port. Another program may be using it."));
	} else {
		obs_log(LOG_INFO, "listening on TCP %d", port_);
	}
	connect(server_, &QTcpServer::newConnection, this, &Hub::onNewConnection);

	udp_ = new QUdpSocket(this);
	if (!udp_->bind(QHostAddress::AnyIPv4, kDiscoveryPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint))
		obs_log(LOG_WARNING, "discovery port %d busy; typed codes won't work", kDiscoveryPort);
	connect(udp_, &QUdpSocket::readyRead, this, &Hub::onDatagram);
}

void Hub::onNewConnection()
{
	while (server_->hasPendingConnections())
		new PhoneLink(server_->nextPendingConnection(), this);
}

// ---- discovery and pairing --------------------------------------------------------------

std::vector<std::string> Hub::hosts() const
{
	struct Cand {
		int rank;
		std::string ip;
	};
	std::vector<Cand> c;
	for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
		auto f = i.flags();
		if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning) ||
		    (f & QNetworkInterface::IsLoopBack))
			continue;
		const QString name = i.humanReadableName() + " " + i.name();
		if (i.type() == QNetworkInterface::Virtual || name.contains("vEthernet", Qt::CaseInsensitive) ||
		    name.contains("VirtualBox", Qt::CaseInsensitive) || name.contains("VMware", Qt::CaseInsensitive) ||
		    name.contains("WSL", Qt::CaseInsensitive) || name.contains("Bluetooth", Qt::CaseInsensitive))
			continue;
		int rank = i.type() == QNetworkInterface::Wifi ? 0 : i.type() == QNetworkInterface::Ethernet ? 1 : 2;
		for (const QNetworkAddressEntry &e : i.addressEntries()) {
			QHostAddress a = e.ip();
			if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLinkLocal() || a.isLoopback())
				continue;
			c.push_back({rank, a.toString().toStdString()});
		}
	}
	std::stable_sort(c.begin(), c.end(), [](const Cand &a, const Cand &b) { return a.rank < b.rank; });
	std::vector<std::string> out;
	for (auto &x : c)
		if (out.size() < 4 && std::find(out.begin(), out.end(), x.ip) == out.end())
			out.push_back(x.ip);
	return out;
}

Pairing Hub::pairing() const
{
	Pairing p;
	p.hosts = hosts();
	p.port = port_;
	p.secret = secret_;
	p.pcId = pcId_;
	p.pcName = pcName();
	return p;
}

std::string Hub::appUrl() const
{
	auto h = hosts();
	if (h.empty())
		return "";
	return "http://" + h.front() + ":" + std::to_string(port_) + "/";
}

std::string Hub::newCode()
{
	std::string c = randomString(kCodeAlphabet, 6);
	{
		std::lock_guard<std::mutex> l(m_);
		code_ = c;
	}
	return c.substr(0, 3) + "-" + c.substr(3);
}

void Hub::endCode()
{
	std::lock_guard<std::mutex> l(m_);
	code_.clear();
}

void Hub::onDatagram()
{
	while (udp_->hasPendingDatagrams()) {
		QNetworkDatagram d = udp_->receiveDatagram(512);
		const std::string s = d.data().toStdString();
		std::string reply;
		if (s.rfind("RAC1?C", 0) == 0) {
			std::lock_guard<std::mutex> l(m_);
			if (!code_.empty() && normaliseCode(s.substr(6)) == code_)
				reply = "1";
		} else if (s.rfind("RAC1?I", 0) == 0 && s.substr(6) == pcId_) {
			reply = "2";
		}
		if (reply == "1") {
			reply = pairing().toJson();
		} else if (reply == "2") {
			json j = {{"v", kProtocolVersion}, {"h", hosts()}, {"p", port_}, {"id", pcId_}};
			reply = j.dump();
		}
		if (!reply.empty())
			udp_->writeDatagram(QByteArray::fromStdString(reply), d.senderAddress(), quint16(d.senderPort()));
	}
}

// ---- views for the dock -----------------------------------------------------------------

std::vector<PhoneView> Hub::phones() const
{
	std::lock_guard<std::mutex> l(m_);
	std::vector<PhoneView> out;
	for (auto &kv : views_)
		out.push_back(kv.second);
	// Phones with a source but no connection show as offline.
	for (auto *s : sources_) {
		const std::string d = s->device();
		if (d.empty())
			continue;
		auto it = std::find_if(out.begin(), out.end(), [&](const PhoneView &v) { return v.device == d; });
		if (it == out.end()) {
			PhoneView v;
			v.device = d;
			v.name = obs_source_get_name(s->obs());
			v.sourceName = v.name;
			out.push_back(v);
		} else if (it->sourceName.empty()) {
			it->sourceName = obs_source_get_name(s->obs());
		}
	}
	return out;
}

bool Hub::hasPhone(const std::string &device) const
{
	std::lock_guard<std::mutex> l(m_);
	auto it = views_.find(device);
	return it != views_.end() && it->second.connected;
}

void Hub::viewChanged(const PhoneView &v, bool gone)
{
	{
		std::lock_guard<std::mutex> l(m_);
		if (gone)
			views_.erase(v.device);
		else
			views_[v.device] = v;
	}
	emit phonesChanged();
}

// ---- commands ----------------------------------------------------------------------------

void Hub::sendCmd(const std::string &device, const std::string &text)
{
	QMetaObject::invokeMethod(
		this,
		[this, device, text] {
			PhoneLink *link = nullptr;
			{
				std::lock_guard<std::mutex> l(m_);
				auto it = links_.find(device);
				if (it != links_.end())
					link = it->second;
			}
			if (link)
				link->sendText(Type::Cmd, text);
		},
		Qt::QueuedConnection);
}

void Hub::setZoom(const std::string &device, double zoom)
{
	sendCmd(device, json{{"zoom", std::round(zoom * 1000) / 1000}}.dump());
}

void Hub::nudgeZoom(const std::string &device, double factor)
{
	PhoneView v;
	{
		std::lock_guard<std::mutex> l(m_);
		auto it = views_.find(device);
		if (it == views_.end())
			return;
		v = it->second;
	}
	const Cam *c = v.caps.cam(v.state.cam);
	if (!c || !c->canZoom())
		return;
	setZoom(device, std::clamp(v.state.zoom * factor, c->min, c->max));
}

void Hub::preset(const std::string &device, int index)
{
	PhoneView v;
	{
		std::lock_guard<std::mutex> l(m_);
		auto it = views_.find(device);
		if (it == views_.end())
			return;
		v = it->second;
	}
	const Cam *c = v.caps.cam(v.state.cam);
	if (!c)
		return;
	auto p = presets(c->min, c->max, c->lenses);
	if (index >= 0 && index < int(p.size()))
		setZoom(device, p[size_t(index)]);
}

void Hub::toggle(const std::string &device, const char *key)
{
	PhoneView v;
	{
		std::lock_guard<std::mutex> l(m_);
		auto it = views_.find(device);
		if (it == views_.end())
			return;
		v = it->second;
	}
	const std::string k = key;
	bool cur = k == "torch" ? v.state.torch
		   : k == "mirror" ? v.state.mirror
		   : k == "flip" ? v.state.flip
		   : k == "lock" ? v.state.lock
		   : k == "mic" ? v.state.mic
				: false;
	sendCmd(device, json{{k, !cur}}.dump());
}

void Hub::nextCam(const std::string &device)
{
	PhoneView v;
	{
		std::lock_guard<std::mutex> l(m_);
		auto it = views_.find(device);
		if (it == views_.end())
			return;
		v = it->second;
	}
	auto &cams = v.caps.cams;
	if (cams.size() < 2)
		return;
	size_t i = 0;
	for (; i < cams.size(); i++)
		if (cams[i].id == v.state.cam)
			break;
	sendCmd(device, json{{"cam", cams[(i + 1) % cams.size()].id}}.dump());
}

// ---- sources ------------------------------------------------------------------------------

void Hub::registerSource(PhoneSource *s)
{
	{
		std::lock_guard<std::mutex> l(m_);
		sources_.push_back(s);
		auto it = views_.find(s->device());
		if (it != views_.end()) {
			s->setConnected(it->second.connected);
			s->setTransform(it->second.state.rot, it->second.state.mirror, it->second.state.flip);
		}
	}
	emit phonesChanged();
}

void Hub::unregisterSource(PhoneSource *s)
{
	{
		std::lock_guard<std::mutex> l(m_);
		sources_.erase(std::remove(sources_.begin(), sources_.end(), s), sources_.end());
	}
	emit phonesChanged();
}

void Hub::sourceChanged()
{
	{
		std::lock_guard<std::mutex> l(m_);
		for (auto *s : sources_) {
			auto it = views_.find(s->device());
			s->setConnected(it != views_.end() && it->second.connected);
		}
	}
	emit phonesChanged();
}

void Hub::deliverPicture(const std::string &device, Picture &p)
{
	std::lock_guard<std::mutex> l(m_);
	for (auto *s : sources_)
		if (s->device() == device) {
			s->pushPicture(p);
			return;
		}
}

void Hub::deliverAudio(const std::string &device, const float *const *planes, int channels, int frames, int rate)
{
	std::lock_guard<std::mutex> l(m_);
	for (auto *s : sources_) {
		if (s->device() != device)
			continue;
		obs_source_audio a = {};
		for (int c = 0; c < channels && c < MAX_AV_PLANES; c++)
			a.data[c] = reinterpret_cast<const uint8_t *>(planes[c]);
		a.frames = uint32_t(frames);
		a.speakers = channels >= 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
		a.format = AUDIO_FORMAT_FLOAT_PLANAR;
		a.samples_per_sec = uint32_t(rate);
		a.timestamp = os_gettime_ns();
		obs_source_output_audio(s->obs(), &a);
		return;
	}
}

void Hub::deliverState(const std::string &device, const State &st)
{
	std::lock_guard<std::mutex> l(m_);
	for (auto *s : sources_)
		if (s->device() == device) {
			s->setTransform(st.rot, st.mirror, st.flip);
			s->remember(st);
		}
}

void Hub::deliverConnected(const std::string &device, bool on)
{
	std::lock_guard<std::mutex> l(m_);
	for (auto *s : sources_)
		if (s->device() == device)
			s->setConnected(on);
}

std::string Hub::restoreFor(const std::string &device)
{
	std::lock_guard<std::mutex> l(m_);
	for (auto *s : sources_)
		if (s->device() == device)
			return s->restoreJson();
	return "{}";
}

/**
 * Adds a source for a phone that has none (the one change to scenes the plugin may make). Runs on
 * the UI thread, fitted to the canvas, so a turn of the phone refits it instead of overflowing.
 */
static void createSourceFor(const std::string &device, const std::string &name)
{
	for (auto &v : Hub::get().phones())
		if (v.device == device && !v.sourceName.empty())
			return;
	const std::string base = "Phone - " + (name.empty() ? std::string("camera") : name);
	std::string n = base;
	for (int i = 2;; i++) {
		obs_source_t *e = obs_get_source_by_name(n.c_str());
		if (!e)
			break;
		obs_source_release(e);
		n = base + " " + std::to_string(i);
	}
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "device", device.c_str());
	obs_source_t *src = obs_source_create(kSourceId, n.c_str(), s, nullptr);
	obs_data_release(s);
	if (!src)
		return;
	if (obs_source_t *sceneSrc = obs_frontend_get_current_scene()) {
		if (obs_scene_t *scene = obs_scene_from_source(sceneSrc)) {
			if (obs_sceneitem_t *item = obs_scene_add(scene, src)) {
				obs_video_info ovi;
				if (obs_get_video_info(&ovi)) {
					vec2 b;
					vec2_set(&b, float(ovi.base_width), float(ovi.base_height));
					obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
					obs_sceneitem_set_bounds(item, &b);
					obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
				}
			}
		}
		obs_source_release(sceneSrc);
	}
	obs_source_release(src);
	obs_log(LOG_INFO, "added source '%s' for a new phone", n.c_str());
}

void Hub::ensureSource(const std::string &device, const std::string &name)
{
	{
		std::lock_guard<std::mutex> l(m_);
		for (auto *s : sources_)
			if (s->device() == device)
				return;
	}
	QMetaObject::invokeMethod(QCoreApplication::instance(), [device, name] { createSourceFor(device, name); },
				  Qt::QueuedConnection);
}

// ============================================================================================
// PhoneLink
// ============================================================================================

PhoneLink::PhoneLink(QTcpSocket *sock, Hub *hub) : QObject(hub), sock_(sock), hub_(hub)
{
	sock_->setParent(this);
	sock_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
	sock_->setReadBufferSize(0);
	lastHeard_ = opened_ = nowMs();
	connect(sock_, &QTcpSocket::readyRead, this, &PhoneLink::onReadyRead);
	connect(sock_, &QTcpSocket::disconnected, this, [this] { close(false); });
	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, &PhoneLink::onTick);
	timer_->start(1000);
	worker_ = std::thread(&PhoneLink::workerLoop, this);
}

PhoneLink::~PhoneLink()
{
	{
		std::lock_guard<std::mutex> l(qm_);
		quit_ = true;
	}
	qcv_.notify_all();
	if (worker_.joinable())
		worker_.join();
}

void PhoneLink::sendText(Type t, const std::string &text)
{
	if (closing_ || !sock_)
		return;
	auto bytes = encodeText(uint8_t(t), text);
	sock_->write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size()));
}

void PhoneLink::close(bool sayBye)
{
	if (closing_)
		return;
	if (sayBye && authed_) {
		auto bye = encode(uint8_t(Type::Bye), {});
		sock_->write(reinterpret_cast<const char *>(bye.data()), qint64(bye.size()));
		sock_->flush();
		sock_->waitForBytesWritten(300);
	}
	closing_ = true;
	timer_->stop();
	{
		std::lock_guard<std::mutex> l(qm_);
		quit_ = true;
		jobs_.clear();
	}
	qcv_.notify_all();
	if (worker_.joinable())
		worker_.join();
	if (authed_) {
		bool mine = false;
		{
			std::lock_guard<std::mutex> l(hub_->m_);
			auto it = hub_->links_.find(device_);
			if (it != hub_->links_.end() && it->second == this) {
				hub_->links_.erase(it);
				mine = true;
			}
		}
		if (mine) {
			hub_->deliverConnected(device_, false);
			hub_->viewChanged(view_, true);
			obs_log(LOG_INFO, "phone '%s' disconnected", name_.c_str());
		}
	}
	sock_->abort();
	deleteLater();
}

void PhoneLink::onReadyRead()
{
	if (closing_)
		return;
	QByteArray b = sock_->readAll();
	if (b.isEmpty())
		return;
	bytes_ += uint64_t(b.size());
	lastHeard_ = nowMs();
	if (!authed_ && !http_ && reader_.buffered() == 0 && b[0] == 'G')
		http_ = true;
	if (http_) {
		httpBuf_.append(b.constData(), size_t(b.size()));
		if (httpBuf_.find("\r\n\r\n") != std::string::npos || httpBuf_.size() > 8192)
			serveHttp();
		return;
	}
	if (!reader_.feed(reinterpret_cast<const uint8_t *>(b.constData()), size_t(b.size()))) {
		close(false);
		return;
	}
	Frame f;
	while (!closing_ && reader_.next(f))
		handle(f);
}

void PhoneLink::handle(Frame &f)
{
	if (!authed_) {
		if (f.type != uint8_t(Type::Hello)) {
			close(false);
			return;
		}
		handleHello(f);
		return;
	}
	switch (Type(f.type)) {
	case Type::Caps:
		view_.caps = Caps::fromJson(f.text());
		publish();
		break;
	case Type::State:
		view_.state = State::fromJson(f.text());
		hub_->deliverState(device_, view_.state);
		publish();
		break;
	case Type::Ping:
		sendText(Type::Pong, f.text());
		break;
	case Type::Pong:
		if (f.payload.size() == 8) {
			uint64_t t = 0;
			for (int i = 0; i < 8; i++)
				t = (t << 8) | f.payload[size_t(i)];
			view_.rttMs = int((nowUs() - int64_t(t)) / 1000);
		}
		break;
	case Type::Bye:
	case Type::Error:
		close(false);
		break;
	case Type::VConf:
	case Type::Video:
	case Type::AConf:
	case Type::Audio:
		enqueue(Type(f.type), std::move(f.payload));
		break;
	default:
		break;
	}
}

void PhoneLink::handleHello(const Frame &f)
{
	json j = json::parse(f.text(), nullptr, false);
	if (!j.is_object() || j.value("v", 0) != kProtocolVersion) {
		sendText(Type::Error, json{{"code", "version"},
					   {"msg", "This PC's Rely-A-Cam plugin is a different version. Update both."}}
					      .dump());
		sock_->flush();
		close(false);
		return;
	}
	if (j.value("k", "") != hub_->secret()) {
		sendText(Type::Error,
			 json{{"code", "auth"}, {"msg", "This PC doesn't know this phone any more. Scan its QR again."}}.dump());
		sock_->flush();
		close(false);
		return;
	}
	device_ = j.value("device", "");
	name_ = j.value("name", "Phone");
	if (device_.empty()) {
		close(false);
		return;
	}
	// A phone back after a drop replaces its old, dead connection.
	PhoneLink *old = nullptr;
	{
		std::lock_guard<std::mutex> l(hub_->m_);
		auto it = hub_->links_.find(device_);
		if (it != hub_->links_.end())
			old = it->second;
	}
	if (old && old != this)
		old->close(false);
	{
		std::lock_guard<std::mutex> l(hub_->m_);
		hub_->links_[device_] = this;
	}
	authed_ = true;
	QString peer = sock_->peerAddress().toString();
	if (peer.startsWith("::ffff:"))
		peer = peer.mid(7);
	view_.device = device_;
	view_.name = name_;
	view_.peer = peer.toStdString();
	view_.usb = peer == "127.0.0.1";
	view_.connected = true;

	hub_->ensureSource(device_, name_);
	json restore = json::parse(hub_->restoreFor(device_), nullptr, false);
	if (!restore.is_object())
		restore = json::object();
	json w = {{"v", kProtocolVersion},
		  {"pc", hub_->pcName()},
		  {"pcId", hub_->pcId()},
		  {"source", "Phone - " + name_},
		  {"codecs", {"h264", "hevc", "aac"}},
		  {"restore", restore}};
	sendText(Type::Welcome, w.dump());
	hub_->deliverConnected(device_, true);
	publish();
	obs_log(LOG_INFO, "phone '%s' connected from %s", name_.c_str(), view_.peer.c_str());
}

/**
 * The "Get the app" page: a phone that scans the dock's second QR lands here and downloads the
 * APK straight from this PC, no internet needed (each half points at the other).
 */
void PhoneLink::serveHttp()
{
	closing_ = true;
	timer_->stop();
	std::string line = httpBuf_.substr(0, httpBuf_.find("\r\n"));
	std::string path = "/";
	if (line.rfind("GET ", 0) == 0) {
		auto end = line.find(' ', 4);
		path = line.substr(4, end == std::string::npos ? std::string::npos : end - 4);
	}
	const std::string apk = hub_->apkPath();
	QByteArray out;
	if (path.rfind("/app.apk", 0) == 0 && !apk.empty()) {
		QFile f(QString::fromStdString(apk));
		if (f.open(QIODevice::ReadOnly)) {
			QByteArray body = f.readAll();
			out = "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.android.package-archive\r\n"
			      "Content-Disposition: attachment; filename=\"RelyACam.apk\"\r\nContent-Length: " +
			      QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
			out += body;
		}
	}
	if (out.isEmpty()) {
		const std::string link = apk.empty() ? "https://github.com/uukjtisa/rely-a-cam/releases/latest" : "/app.apk";
		std::string html =
			"<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
			"<title>Rely-A-Cam</title><style>body{font-family:sans-serif;background:#1d1f26;color:#eee;margin:0;padding:28px;line-height:1.5}"
			"a.b{display:block;background:#284cb8;color:#fff;text-align:center;padding:16px;border-radius:6px;text-decoration:none;font-size:18px;margin:22px 0}"
			"small{color:#999}</style></head><body><h1>Rely-A-Cam</h1>"
			"<p>Turns this phone into a camera for OBS on <b>" +
			hub_->pcName() + "</b>.</p><a class=b href='" + link +
			"'>Download the app</a><p>Open the download to install it. Android may ask you to allow installs from your browser: allow it, then come back.</p>"
			"<p>Then open Rely-A-Cam and scan the other QR in OBS.</p>"
			"<small>Also on GitHub: github.com/uukjtisa/rely-a-cam</small></body></html>";
		out = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
		      QByteArray::number(qint64(html.size())) + "\r\nConnection: close\r\n\r\n" + QByteArray::fromStdString(html);
	}
	sock_->write(out);
	connect(sock_, &QTcpSocket::bytesWritten, this, [this] {
		if (sock_->bytesToWrite() == 0) {
			sock_->disconnectFromHost();
			deleteLater();
		}
	});
	QTimer::singleShot(120000, this, [this] { deleteLater(); });
}

void PhoneLink::publish()
{
	view_.connected = true;
	hub_->viewChanged(view_, false);
}

void PhoneLink::onTick()
{
	const int64_t now = nowMs();
	if (!authed_) {
		if (now - opened_ > 5000)
			close(false);
		return;
	}
	if (now - lastHeard_ > 6000) {
		obs_log(LOG_INFO, "phone '%s' stopped answering", name_.c_str());
		close(false);
		return;
	}
	uint8_t p[8];
	const uint64_t t = uint64_t(nowUs());
	for (int i = 0; i < 8; i++)
		p[i] = uint8_t(t >> ((7 - i) * 8));
	auto bytes = encode(uint8_t(Type::Ping), std::vector<uint8_t>(p, p + 8));
	sock_->write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size()));
	view_.mbps = double(bytes_ - lastBytes_) * 8.0 / 1e6;
	lastBytes_ = bytes_;
	publish();
}

void PhoneLink::enqueue(Type t, std::vector<uint8_t> &&p)
{
	bool askKey = false;
	{
		std::lock_guard<std::mutex> l(qm_);
		if (t == Type::Video) {
			const bool key = p.size() > 8 && (p[8] & (kFlagKey | kFlagConfig));
			if (waitKey_ && !key)
				return;
			if (key)
				waitKey_ = false;
			size_t videos = size_t(std::count_if(jobs_.begin(), jobs_.end(), [](const Job &j) { return j.type == Type::Video; }));
			if (videos > 12) {
				// Decoding fell behind: skip to the next key frame rather than run late (D-05).
				jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Job &j) { return j.type == Type::Video; }),
					    jobs_.end());
				if (!key) {
					waitKey_ = true;
					askKey = true;
				}
			}
			if (waitKey_)
				p.clear();
		}
		if (!p.empty() || t != Type::Video)
			jobs_.push_back({t, std::move(p)});
	}
	qcv_.notify_one();
	if (askKey)
		sendText(Type::Cmd, R"({"keyframe":true})");
}

void PhoneLink::workerLoop()
{
	Picture pic, small;
	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> l(qm_);
			qcv_.wait(l, [this] { return quit_ || !jobs_.empty(); });
			if (quit_)
				return;
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}
		switch (job.type) {
		case Type::VConf: {
			json j = json::parse(std::string(job.payload.begin(), job.payload.end()), nullptr, false);
			const std::string codec = j.is_object() ? j.value("codec", "h264") : "h264";
			if (!vdec_.open(codec)) {
				obs_log(LOG_WARNING, "no %s decoder", codec.c_str());
				emit hub_->message(QString("This PC can't decode %1. Pick H.264 on the phone.").arg(QString::fromStdString(codec)));
			}
			break;
		}
		case Type::Video: {
			Media m;
			if (!parseMedia(job.payload, m))
				break;
			if (!vdec_.isOpen())
				vdec_.open("h264");
			const int64_t now = nowMs();
			const bool wantSmall = hub_->previewWanted() && now - lastPreview_ >= 50;
			if (vdec_.decode(m.data, m.size, pic, wantSmall ? &small : nullptr, 640)) {
				hub_->deliverPicture(device_, pic);
				if (wantSmall && small.width > 0) {
					lastPreview_ = now;
					QImage img(small.bgra.data(), small.width, small.height, small.linesize, QImage::Format_RGB32);
					hub_->emitPreview(device_, img.copy());
				}
			}
			break;
		}
		case Type::AConf: {
			json j = json::parse(std::string(job.payload.begin(), job.payload.end()), nullptr, false);
			if (j.is_object()) {
				audioRate_ = j.value("rate", 48000);
				audioCh_ = j.value("ch", 1);
			}
			break;
		}
		case Type::Audio: {
			Media m;
			if (!parseMedia(job.payload, m))
				break;
			if (m.flags & kFlagConfig) {
				adec_.open(m.data, m.size, audioRate_, audioCh_);
			} else if (adec_.isOpen()) {
				adec_.decode(m.data, m.size, [this](const float *const *planes, int ch, int frames, int rate) {
					hub_->deliverAudio(device_, planes, ch, frames, rate);
				});
			}
			break;
		}
		default:
			break;
		}
	}
}

} // namespace rac
