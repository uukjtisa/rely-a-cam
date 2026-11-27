// SPDX-License-Identifier: GPL-2.0-or-later
#include "link.hpp"

#include "../net/hub.hpp"
#include "../net/protocol.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QTimer>
#include <QTransform>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using nlohmann::json;

namespace rac {

namespace {

// The link's numbers (Rely-A-Cam-Gestures src/link/wire.hpp; docs/LINK.md).
constexpr int kVersion = 1;
namespace msg {
enum : uint8_t {
	Hello = 0x01,
	Phones = 0x02,
	Frame = 0x03,
	Phone = 0x04,
	Obs = 0x05,
	Cmd = 0x06,
	LibSet = 0x07,
	Upload = 0x08,
	Bye = 0x0F,
	Layer = 0x10,
	Action = 0x12,
	Event = 0x13,
	Audio = 0x14,
	Status = 0x15,
	Det = 0x16,
	Lib = 0x17,
};
} // namespace msg
constexpr uint32_t kMaxMessage = 32u * 1024u * 1024u;
constexpr uint32_t kInSlotBytes = 640u * 640u * 4u;
constexpr uint32_t kHeaderBytes = 64;
constexpr int64_t kFrameMs = 66;   // 15 a second is what the app sees at

struct BlockHeader {
	char magic[8];
	uint32_t slotBytes;
	uint32_t reserved;
	uint64_t seq[2];
	uint32_t w[2], h[2], stride[2];
};

std::wstring userName()
{
	wchar_t user[256] = {};
	DWORD n = 256;
	GetUserNameW(user, &n);
	return user;
}

QString pipeName()
{
	return QStringLiteral("Rely-A-Cam-Gestures-link-") + QString::fromStdWString(userName());
}

QString blockName(const char *direction, const std::string &device)
{
	uint32_t h = 2166136261u;
	for (const char c : device) {
		h ^= uint8_t(c);
		h *= 16777619u;
	}
	return QStringLiteral("Local\\RACG.%1.%2").arg(direction).arg(h, 8, 16, QChar('0'));
}

/** The installed app: RACG_EXE (a dev build), else where its installer said it put it. */
QString exePath()
{
	const QString env = qEnvironmentVariable("RACG_EXE");
	if (!env.isEmpty() && QFileInfo::exists(env))
		return env;
	for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
		wchar_t buf[MAX_PATH] = {};
		DWORD size = sizeof(buf);
		if (RegGetValueW(root, L"Software\\Rely-A-Cam-Gestures", L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &size) ==
		    ERROR_SUCCESS) {
			const QString exe = QDir(QString::fromWCharArray(buf)).filePath("Rely-A-Cam-Gestures.exe");
			if (QFileInfo::exists(exe))
				return exe;
		}
	}
	return {};
}

std::string configPath()
{
	char *p = obs_module_config_path("gestures.json");
	std::string s = p ? p : "gestures.json";
	bfree(p);
	return s;
}

// ---- the Phone Memes source ------------------------------------------------------------------

constexpr const char *kMemesId = "rely_a_cam_memes";
constexpr uint32_t kMemesChannel = 63;   // the last global channel: clear of OBS's 1-6 and the mics from 7
std::mutex memesMutex;
obs_source_t *memesSource = nullptr;     // weak: the source registers itself while it exists

/** The Phone Memes source, made and put in every scene the first time a sound arrives. UI thread. */
void ensureMemesSource()
{
	{
		std::lock_guard<std::mutex> l(memesMutex);
		if (memesSource)
			return;
	}
	obs_source_t *src = obs_get_source_by_name("Phone Memes");
	if (!src || strcmp(obs_source_get_id(src), kMemesId) != 0) {
		obs_source_release(src);
		src = obs_source_create(kMemesId, "Phone Memes", nullptr, nullptr);
	}
	if (!src)
		return;
	// Not in a scene: heard everywhere through a global channel, like Desktop Audio.
	obs_source_t *onChannel = obs_get_output_source(kMemesChannel);
	if (onChannel != src)
		obs_set_output_source(kMemesChannel, src);
	obs_source_release(onChannel);
	obs_source_release(src);
}

} // namespace

struct Gestures::Block {
	HANDLE handle = nullptr;
	uint8_t *view = nullptr;
	~Block()
	{
		if (view)
			UnmapViewOfFile(view);
		if (handle)
			CloseHandle(handle);
	}
	bool create(const QString &name, uint32_t slotBytes)
	{
		const uint64_t size = uint64_t(kHeaderBytes) + 2ull * slotBytes;
		handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(size >> 32), DWORD(size),
					    reinterpret_cast<LPCWSTR>(name.utf16()));
		if (!handle)
			return false;
		view = static_cast<uint8_t *>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0));
		if (!view)
			return false;
		auto *h = header();
		memset(h, 0, sizeof(BlockHeader));
		memcpy(h->magic, "RACGLNK1", 8);
		h->slotBytes = slotBytes;
		return true;
	}
	bool open(const QString &name)
	{
		handle = OpenFileMappingW(FILE_MAP_READ, FALSE, reinterpret_cast<LPCWSTR>(name.utf16()));
		if (!handle)
			return false;
		view = static_cast<uint8_t *>(MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0));
		return view && memcmp(view, "RACGLNK1", 8) == 0;
	}
	BlockHeader *header() const { return reinterpret_cast<BlockHeader *>(view); }
	uint8_t *slot(int i) const { return view + kHeaderBytes + size_t(i) * header()->slotBytes; }
};

Gestures &Gestures::get()
{
	static Gestures *g = new Gestures(); // lives until OBS exits; Qt objects die with the app
	return *g;
}

void Gestures::start()
{
	if (started_)
		return;
	started_ = true;
	loadConfig();
	timer_ = new QTimer(this);
	timer_->setInterval(1000);
	connect(timer_, &QTimer::timeout, this, &Gestures::tick);
	timer_->start();
	connect(&Hub::get(), &Hub::phonesChanged, this, [this] {
		// How each phone's source turns its picture (the decode thread reads it).
		{
			std::lock_guard<std::mutex> l(m_);
			for (const auto &v : Hub::get().phones())
				poses_[v.device] = {v.state.rot, v.state.mirror, v.state.flip};
		}
		if (hello_) {
			sendPhones();
			sendPhoneStates();
		}
	});
	obs_frontend_add_event_callback(
		[](enum obs_frontend_event e, void *self) { Gestures::frontendEvent(int(e), self); }, this);
	tick();
}

void Gestures::stop()
{
	if (!started_)
		return;
	started_ = false;
	if (timer_)
		timer_->stop();
	if (sock_ && hello_) {
		send(msg::Bye, json{{"reason", "obs closing"}}.dump());
		sock_->flush();
		sock_->waitForBytesWritten(300);
	}
	if (sock_)
		sock_->abort();
	connected_ = false;
	hello_ = false;
}

void Gestures::frontendEvent(int event, void *self)
{
	auto *g = static_cast<Gestures *>(self);
	switch (enum obs_frontend_event(event)) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		if (g->hello_)
			g->sendObs();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		g->stop();
		break;
	default:
		break;
	}
}

// ---- settings --------------------------------------------------------------------------------

void Gestures::loadConfig()
{
	QFile f(QString::fromStdString(configPath()));
	if (!f.open(QIODevice::ReadOnly))
		return;
	json j = json::parse(f.readAll().toStdString(), nullptr, false);
	if (!j.is_object())
		return;
	std::lock_guard<std::mutex> l(m_);
	if (j.contains("on") && j["on"].is_array())
		for (auto &d : j["on"])
			if (d.is_string())
				on_.insert(d.get<std::string>());
}

void Gestures::saveConfig()
{
	json j;
	{
		std::lock_guard<std::mutex> l(m_);
		j["on"] = on_;
	}
	QFile f(QString::fromStdString(configPath()));
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(QByteArray::fromStdString(j.dump()));
}

bool Gestures::on(const std::string &device) const
{
	std::lock_guard<std::mutex> l(m_);
	return on_.count(device) > 0;
}

void Gestures::setOn(const std::string &device, bool on)
{
	{
		std::lock_guard<std::mutex> l(m_);
		if (on)
			on_.insert(device);
		else
			on_.erase(device);
		if (!on)
			overlays_.erase(device);
	}
	saveConfig();
	if (!on)
		Hub::get().deliverLayer(device, QImage(), QRectF(0, 0, 1, 1));
	if (hello_) {
		sendPhones();
		sendPhoneStates();
	}
	tick();
	pushLibrary(device);
	emit changed();
}

void Gestures::fromPhone(const std::string &device, bool file, const std::string &payload)
{
	if (file) {
		if (hello_)
			send(msg::Upload, payload);
		return;
	}
	json j = json::parse(payload, nullptr, false);
	if (!j.is_object())
		return;
	if (j.contains("on") && j["on"].is_boolean()) {
		setOn(device, j["on"].get<bool>());
		return;
	}
	if (j.value("openEditor", false)) {
		openEditor(device);
		return;
	}
	// Which phone asked, so a take is recorded from that phone's camera.
	j["device"] = device;
	if (hello_)
		send(msg::LibSet, j.dump());
}

void Gestures::pushLibrary(const std::string &device)
{
	json lib = json::parse(lastLib_, nullptr, false);
	if (!lib.is_object())
		lib = json::object();
	for (const auto &v : Hub::get().phones()) {
		if (!v.connected || (!device.empty() && v.device != device))
			continue;
		json j = lib;
		j["installed"] = installed();
		j["on"] = on(v.device);
		j["linked"] = bool(hello_);
		j["status"] = status().toStdString();
		Hub::get().sendGesture(v.device, Type::GLib, j.dump());
	}
}

bool Gestures::installed() const
{
	return !exePath().isEmpty();
}

QString Gestures::status() const
{
	if (!installed())
		return QStringLiteral("Gestures app not installed: rerun the Rely-A-Cam installer with Gestures ticked");
	if (!connected_)
		return launches_ > 3 ? QStringLiteral("The gestures app didn't start; trying again")
				     : QStringLiteral("Starting the gestures app...");
	if (!error_.isEmpty())
		return QStringLiteral("Gestures stopped: ") + error_;
	if (!ready_)
		return QStringLiteral("Loading the gesture models...");
	return QStringLiteral("Gestures ready (%1)").arg(device_.isEmpty() ? QStringLiteral("CPU") : device_);
}

void Gestures::setDetection(bool dock, bool output)
{
	if (dock != detDock_)
		send(msg::Cmd, json{{"overlay", {{"which", "detection"}, {"place", "preview"}, {"on", dock}}}}.dump());
	if (output != detOut_)
		send(msg::Cmd, json{{"overlay", {{"which", "detection"}, {"place", "output"}, {"on", output}}}}.dump());
	detDock_ = dock;
	detOut_ = output;
	emit changed();
}

void Gestures::setFaceMap(bool dock, bool output)
{
	if (dock != faceDock_)
		send(msg::Cmd, json{{"overlay", {{"which", "face"}, {"place", "preview"}, {"on", dock}}}}.dump());
	if (output != faceOut_)
		send(msg::Cmd, json{{"overlay", {{"which", "face"}, {"place", "output"}, {"on", output}}}}.dump());
	faceDock_ = dock;
	faceOut_ = output;
	emit changed();
}

void Gestures::openEditor(const std::string &device)
{
	if (hello_) {
		send(msg::Cmd, json{{"openEditor", device}}.dump());
		return;
	}
	// Not linked yet: just start it with its window.
	const QString exe = exePath();
	if (!exe.isEmpty())
		QProcess::startDetached(exe, {});
}

GestureOverlay Gestures::overlay(const std::string &device) const
{
	std::lock_guard<std::mutex> l(m_);
	auto it = overlays_.find(device);
	return it == overlays_.end() ? GestureOverlay() : it->second;
}

// ---- the connection --------------------------------------------------------------------------

void Gestures::tick()
{
	const auto phones = Hub::get().phones();
	bool want = false;
	{
		std::lock_guard<std::mutex> l(m_);
		for (const auto &v : phones)
			want |= v.connected && on_.count(v.device) > 0;
	}
	if (want && !sock_)
		tryConnect();
	if (!want && sock_ && hello_) {
		// No phone wants gestures: let the app go (it quits if Rely-A-Cam started it).
		send(msg::Bye, json{{"reason", "no phones"}}.dump());
		sock_->flush();
		sock_->disconnectFromServer();
	}
}

void Gestures::tryConnect()
{
	const int64_t now = int64_t(os_gettime_ns() / 1000000);
	if (now - triedAt_ < 900)
		return;
	triedAt_ = now;
	sock_ = new QLocalSocket(this);
	connect(sock_, &QLocalSocket::connected, this, &Gestures::onConnected);
	connect(sock_, &QLocalSocket::disconnected, this, &Gestures::onDisconnected);
	connect(sock_, &QLocalSocket::readyRead, this, &Gestures::onReadyRead);
	connect(sock_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
		if (!connected_) {
			// Not running: start it (at most every 20 s, backing off after a few tries).
			const int64_t t = int64_t(os_gettime_ns() / 1000000);
			const int64_t wait = launches_ > 3 ? 30000 : 20000;
			if (t - launchedAt_ > wait)
				launch();
		}
		onDisconnected();
	});
	sock_->connectToServer(pipeName());
}

void Gestures::launch()
{
	const QString exe = exePath();
	if (exe.isEmpty())
		return;
	launchedAt_ = int64_t(os_gettime_ns() / 1000000);
	launches_++;
	obs_log(LOG_INFO, "starting the gestures app: %s", exe.toUtf8().constData());
	QProcess::startDetached(exe, {QStringLiteral("--attach"), QStringLiteral("--hidden")});
	emit changed();
}

void Gestures::onConnected()
{
	connected_ = true;
	buf_.clear();
	send(msg::Hello, json{{"v", kVersion}, {"app", "rely-a-cam"}, {"version", PLUGIN_VERSION}}.dump());
}

void Gestures::onDisconnected()
{
	const bool was = hello_;
	if (sock_) {
		sock_->disconnect(this);
		sock_->deleteLater();
		sock_ = nullptr;
	}
	connected_ = false;
	hello_ = false;
	ready_ = false;
	std::vector<std::string> devices;
	{
		std::lock_guard<std::mutex> l(m_);
		for (auto &[d, o] : overlays_)
			devices.push_back(d);
		overlays_.clear();
		in_.clear();
		out_.clear();
	}
	for (auto &d : devices)
		Hub::get().deliverLayer(d, QImage(), QRectF(0, 0, 1, 1));
	if (was) {
		obs_log(LOG_INFO, "the gestures app let go");
		emit changed();
	}
}

void Gestures::send(uint8_t type, const std::string &payload)
{
	if (!sock_ || !connected_)
		return;
	char head[5];
	const uint32_t n = uint32_t(payload.size());
	head[0] = char(type);
	head[1] = char(n >> 24);
	head[2] = char(n >> 16);
	head[3] = char(n >> 8);
	head[4] = char(n);
	sock_->write(head, 5);
	sock_->write(payload.data(), qint64(payload.size()));
}

void Gestures::onReadyRead()
{
	const QByteArray in = sock_->readAll();
	buf_.append(in.constData(), size_t(in.size()));
	while (buf_.size() >= 5) {
		const uint32_t len = (uint32_t(uint8_t(buf_[1])) << 24) | (uint32_t(uint8_t(buf_[2])) << 16) |
				     (uint32_t(uint8_t(buf_[3])) << 8) | uint32_t(uint8_t(buf_[4]));
		if (len > kMaxMessage) {
			obs_log(LOG_WARNING, "gestures: oversized message; dropping the link");
			sock_->abort();
			return;
		}
		if (buf_.size() < 5 + size_t(len))
			return;
		const uint8_t type = uint8_t(buf_[0]);
		std::string payload = buf_.substr(5, len);
		buf_.erase(0, 5 + size_t(len));
		handle(type, payload);
		if (!sock_)
			return;
	}
}

void Gestures::handle(uint8_t type, const std::string &payload)
{
	if (type == msg::Audio) {
		onAudio(payload);
		return;
	}
	json j = json::parse(payload, nullptr, false);
	if (!j.is_object())
		return;
	switch (type) {
	case msg::Hello:
		if (j.value("v", 0) != kVersion) {
			obs_log(LOG_WARNING, "gestures: the app speaks link version %d, this plugin %d", j.value("v", 0), kVersion);
			sock_->abort();
			return;
		}
		hello_ = true;
		launches_ = 0;
		appVersion_ = QString::fromStdString(j.value("version", ""));
		obs_log(LOG_INFO, "linked to the gestures app %s", appVersion_.toUtf8().constData());
		sendPhones();
		sendPhoneStates();
		sendObs();
		emit changed();
		break;
	case msg::Bye:
		sock_->disconnectFromServer();
		break;
	case msg::Status: {
		ready_ = j.value("ready", false);
		device_ = QString::fromStdString(j.value("device", ""));
		error_ = QString::fromStdString(j.value("error", ""));
		if (j.contains("overlay") && j["overlay"].contains("detection")) {
			detDock_ = j["overlay"]["detection"].value("dock", false);
			detOut_ = j["overlay"]["detection"].value("output", false);
		}
		if (j.contains("overlay") && j["overlay"].contains("face")) {
			faceDock_ = j["overlay"]["face"].value("dock", false);
			faceOut_ = j["overlay"]["face"].value("output", false);
		}
		pushLibrary();
		emit changed();
		break;
	}
	case msg::Layer:
		onLayer(payload);
		break;
	case msg::Action:
		onAction(payload);
		break;
	case msg::Det: {
		const std::string dev = j.value("device", "");
		// The phone's screen is the other "preview" place: it draws the same hands (PROTOCOL.md, GDET).
		if ((detDock_ || faceDock_) && on(dev)) {
			json p = j;
			p.erase("device");
			Hub::get().sendGesture(dev, Type::GDet, p.dump());
		}
		std::lock_guard<std::mutex> l(m_);
		auto &o = overlays_[dev];
		o.hands.clear();
		o.labels.clear();
		o.face.clear();
		if (j.contains("face") && j["face"].is_array())
			for (auto &l : j["face"]) {
				std::vector<QPointF> pts;
				for (size_t i = 0; i + 1 < l.size(); i += 2)
					pts.emplace_back(l[i].get<double>(), l[i + 1].get<double>());
				o.face.push_back(std::move(pts));
			}
		if (j.contains("hands") && j["hands"].is_array())
			for (auto &h : j["hands"]) {
				std::vector<QPointF> pts;
				for (size_t i = 0; i + 1 < h.size(); i += 2)
					pts.emplace_back(h[i].get<double>(), h[i + 1].get<double>());
				o.hands.push_back(std::move(pts));
			}
		if (j.contains("g") && j["g"].is_array())
			for (auto &g : j["g"])
				o.labels.push_back({QString::fromStdString(g.value("n", "")), QString::fromStdString(g.value("s", "")),
						    QPointF(g.value("x", 0.5), g.value("y", 0.5)), g.value("a", 0.0f),
						    QString::fromStdString(g.value("c", ""))});
		break;
	}
	case msg::Event:
		obs_log(LOG_DEBUG, "gesture %s %s on %s", j.value("name", "").c_str(), j.value("op", "").c_str(),
			j.value("device", "").c_str());
		break;
	case msg::Lib:
		// The phone's Library screen.
		lastLib_ = payload;
		pushLibrary();
		break;
	default:
		break;
	}
}

void Gestures::sendPhones()
{
	json phones = json::array();
	for (const auto &v : Hub::get().phones())
		if (v.connected && on(v.device))
			phones.push_back({{"device", v.device}, {"name", v.name}});
	send(msg::Phones, json{{"phones", phones}}.dump());
}

void Gestures::sendPhoneStates()
{
	for (const auto &v : Hub::get().phones()) {
		if (!v.connected || !on(v.device))
			continue;
		const Cam *c = v.caps.cam(v.state.cam);
		json st = {{"torch", v.state.torch},
			   {"muted", !v.state.mic},
			   {"micOnly", v.state.micOnly},
			   {"front", c && c->facing == "front"},
			   {"zoom", v.state.zoom},
			   {"hasTorch", c && c->flash}};
		send(msg::Phone, json{{"device", v.device}, {"state", st}}.dump());
	}
}

void Gestures::sendObs()
{
	json scenes = json::array();
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	// OBS lists them bottom-up; the Scenes dock shows them top-down.
	for (size_t i = list.sources.num; i-- > 0;)
		scenes.push_back(obs_source_get_name(list.sources.array[i]));
	obs_frontend_source_list_free(&list);
	obs_source_t *cur = obs_frontend_get_current_scene();
	const std::string scene = cur ? obs_source_get_name(cur) : "";
	obs_source_release(cur);
	json hotkeys = json::array();
	obs_enum_hotkeys(
		[](void *data, obs_hotkey_id, obs_hotkey_t *key) {
			auto *arr = static_cast<json *>(data);
			const char *desc = obs_hotkey_get_description(key);
			if (!desc || !*desc)
				return true;
			std::string label = desc;
			if (obs_hotkey_get_registerer_type(key) == OBS_HOTKEY_REGISTERER_SOURCE) {
				obs_source_t *s = obs_weak_source_get_source(
					static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(key)));
				if (s)
					label = std::string(obs_source_get_name(s)) + ": " + label;
				obs_source_release(s);
			}
			arr->push_back(label);
			return true;
		},
		&hotkeys);
	send(msg::Obs, json{{"recording", obs_frontend_recording_active()},
		       {"streaming", obs_frontend_streaming_active()},
		       {"scene", scene},
		       {"scenes", scenes},
		       {"hotkeys", hotkeys}}
			  .dump());
}

// ---- pictures --------------------------------------------------------------------------------

bool Gestures::wantsFrame(const std::string &device)
{
	if (!connected_ || !hello_)
		return false;
	std::lock_guard<std::mutex> l(m_);
	if (!on_.count(device))
		return false;
	const int64_t now = int64_t(os_gettime_ns() / 1000000);
	int64_t &last = lastFrame_[device];
	if (now - last < kFrameMs)
		return false;
	last = now;
	return true;
}

void Gestures::deliverFrame(const std::string &device, const uint8_t *bgra, int w, int h, int stride, int fullW, int fullH)
{
	Pose pose;
	{
		std::lock_guard<std::mutex> l(m_);
		auto it = poses_.find(device);
		if (it != poses_.end())
			pose = it->second;
	}
	// The way the source shows it: turned, then mirrored or flipped (PhoneSource::render).
	QImage img = QImage(bgra, w, h, stride, QImage::Format_RGB32);
	if (pose.rot % 360)
		img = img.transformed(QTransform().rotate(pose.rot));
	if (pose.mirror || pose.flip)
		img = img.mirrored(pose.mirror, pose.flip);
	if (img.format() != QImage::Format_RGB32)
		img = img.convertToFormat(QImage::Format_RGB32);
	const bool side = pose.rot % 180 != 0;
	const int outW = side ? fullH : fullW, outH = side ? fullW : fullH;
	const uint32_t rowBytes = uint32_t(img.width()) * 4;
	if (uint64_t(rowBytes) * uint32_t(img.height()) > kInSlotBytes)
		return;

	uint64_t seq;
	int slot;
	{
		std::lock_guard<std::mutex> l(m_);
		auto &b = in_[device];
		if (!b) {
			b = std::make_unique<Block>();
			if (!b->create(blockName("in", device), kInSlotBytes)) {
				in_.erase(device);
				return;
			}
		}
		seq = ++inSeq_[device];
		slot = int(seq & 1);
		uint8_t *dst = b->slot(slot);
		for (int y = 0; y < img.height(); y++)
			memcpy(dst + size_t(rowBytes) * y, img.constScanLine(y), rowBytes);
		auto *hd = b->header();
		hd->w[slot] = uint32_t(img.width());
		hd->h[slot] = uint32_t(img.height());
		hd->stride[slot] = rowBytes;
		hd->seq[slot] = seq;
	}
	const std::string msg = json{{"device", device}, {"slot", slot},          {"seq", seq},     {"w", img.width()},
				     {"h", img.height()}, {"stride", rowBytes}, {"outW", outW}, {"outH", outH}}
					.dump();
	QMetaObject::invokeMethod(this, [this, msg] { send(msg::Frame, msg); }, Qt::QueuedConnection);
}

void Gestures::onLayer(const std::string &text)
{
	json j = json::parse(text, nullptr, false);
	const std::string dev = j.value("device", "");
	QRectF crop(0, 0, 1, 1);
	if (j.contains("crop") && j["crop"].is_array() && j["crop"].size() == 4)
		crop = QRectF(j["crop"][0].get<double>(), j["crop"][1].get<double>(), j["crop"][2].get<double>(),
			      j["crop"][3].get<double>());
	QImage layer;
	if (!j.value("empty", true)) {
		std::lock_guard<std::mutex> l(m_);
		auto &b = out_[dev];
		if (!b) {
			b = std::make_unique<Block>();
			if (!b->open(blockName("out", dev))) {
				out_.erase(dev);
				return;
			}
		}
		const int slot = j.value("slot", 0) & 1;
		const uint64_t seq = j.value("seq", uint64_t(0));
		const int w = j.value("w", 0), h = j.value("h", 0), stride = j.value("stride", 0);
		if (w <= 0 || h <= 0 || stride < w * 4 || uint64_t(stride) * uint64_t(h) > b->header()->slotBytes)
			return;
		layer = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
		const uint8_t *src = b->slot(slot);
		for (int y = 0; y < h; y++)
			memcpy(layer.scanLine(y), src + size_t(stride) * y, size_t(w) * 4);
		if (b->header()->seq[slot] != seq)
			return; // overwritten mid-copy; the next one is on its way
	}
	{
		std::lock_guard<std::mutex> l(m_);
		auto &o = overlays_[dev];
		o.layer = layer;
		o.crop = crop;
	}
	Hub::get().deliverLayer(dev, layer, crop);
}

void Gestures::onAction(const std::string &text)
{
	json j = json::parse(text, nullptr, false);
	const std::string dev = j.value("device", ""), id = j.value("id", ""), arg = j.value("arg", "");
	Hub &hub = Hub::get();
	PhoneView view;
	bool found = false;
	for (const auto &v : hub.phones())
		if (v.device == dev) {
			view = v;
			found = true;
		}
	if (id.rfind("phone.", 0) == 0 || id == "zoom.step" || id == "snapshot") {
		if (!found || !view.connected)
			return;
		if (id == "phone.torch")
			hub.sendCmd(dev, json{{"torch", arg == "on"}}.dump());
		else if (id == "phone.mic")
			hub.sendCmd(dev, json{{"mic", arg != "on"}}.dump()); // "on" = muted
		else if (id == "phone.miconly")
			hub.sendCmd(dev, json{{"micOnly", arg == "on"}}.dump());
		else if (id == "snapshot")
			hub.sendCmd(dev, json{{"snap", true}}.dump());
		else if (id == "phone.switch") {
			std::string to;
			for (const auto &c : view.caps.cams)
				if (c.facing == arg && to.empty())
					to = c.id;
			if (!to.empty())
				hub.sendCmd(dev, json{{"cam", to}}.dump());
			else
				hub.nextCam(dev);
		} else if (id == "zoom.step") {
			const Cam *c = view.caps.cam(view.state.cam);
			if (!c || !c->canZoom())
				return;
			const auto stops = presets(c->min, c->max, c->lenses);
			if (stops.empty())
				return;
			size_t at = 0;
			for (size_t i = 0; i < stops.size(); i++)
				if (std::fabs(std::log(stops[i] / view.state.zoom)) <
				    std::fabs(std::log(stops[at] / view.state.zoom)))
					at = i;
			const int next = std::clamp(int(at) + (arg == "-1" ? -1 : 1), 0, int(stops.size()) - 1);
			hub.setZoom(dev, stops[size_t(next)]);
		}
		return;
	}
	if (id == "obs.scene") {
		obs_source_t *s = obs_get_source_by_name(arg.c_str());
		if (s && obs_source_is_scene(s))
			obs_frontend_set_current_scene(s);
		obs_source_release(s);
	} else if (id == "obs.record") {
		if (arg == "on" && !obs_frontend_recording_active())
			obs_frontend_recording_start();
		else if (arg == "off" && obs_frontend_recording_active())
			obs_frontend_recording_stop();
	} else if (id == "obs.stream") {
		if (arg == "on" && !obs_frontend_streaming_active())
			obs_frontend_streaming_start();
		else if (arg == "off" && obs_frontend_streaming_active())
			obs_frontend_streaming_stop();
	} else if (id == "obs.hotkey") {
		struct Find {
			std::string label;
			obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
		} find{arg};
		obs_enum_hotkeys(
			[](void *data, obs_hotkey_id hid, obs_hotkey_t *key) {
				auto *f = static_cast<Find *>(data);
				const char *desc = obs_hotkey_get_description(key);
				if (!desc)
					return true;
				std::string label = desc;
				if (obs_hotkey_get_registerer_type(key) == OBS_HOTKEY_REGISTERER_SOURCE) {
					obs_source_t *s = obs_weak_source_get_source(
						static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(key)));
					if (s)
						label = std::string(obs_source_get_name(s)) + ": " + label;
					obs_source_release(s);
				}
				if (label == f->label) {
					f->id = hid;
					return false;
				}
				return true;
			},
			&find);
		if (find.id != OBS_INVALID_HOTKEY_ID) {
			obs_hotkey_trigger_routed_callback(find.id, true);
			obs_hotkey_trigger_routed_callback(find.id, false);
		}
	}
}

void Gestures::onAudio(const std::string &payload)
{
	if (payload.size() < 4)
		return;
	const uint32_t frames = (uint32_t(uint8_t(payload[0])) << 24) | (uint32_t(uint8_t(payload[1])) << 16) |
				(uint32_t(uint8_t(payload[2])) << 8) | uint32_t(uint8_t(payload[3]));
	if (payload.size() < 4 + size_t(frames) * 8)
		return;
	ensureMemesSource();
	std::lock_guard<std::mutex> l(memesMutex);
	if (!memesSource)
		return;
	obs_source_audio a = {};
	a.data[0] = reinterpret_cast<const uint8_t *>(payload.data() + 4);
	a.frames = frames;
	a.speakers = SPEAKERS_STEREO;
	a.format = AUDIO_FORMAT_FLOAT;
	a.samples_per_sec = 48000;
	a.timestamp = os_gettime_ns();
	obs_source_output_audio(memesSource, &a);
}

void registerMemesSource()
{
	static obs_source_info info = {};
	info.id = kMemesId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
	info.get_name = [](void *) { return obs_module_text("PhoneMemes"); };
	info.create = [](obs_data_t *, obs_source_t *src) -> void * {
		std::lock_guard<std::mutex> l(memesMutex);
		if (!memesSource)
			memesSource = src;
		return src;
	};
	info.destroy = [](void *d) {
		std::lock_guard<std::mutex> l(memesMutex);
		if (memesSource == d)
			memesSource = nullptr;
	};
	obs_register_source(&info);
}

} // namespace rac
