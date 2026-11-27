// SPDX-License-Identifier: GPL-2.0-or-later
#include "dock.hpp"

#include "../audio/routing.hpp"
#include "../sys/system.hpp"
#include "../third_party/qrcodegen/qrcodegen.hpp"

#include <obs-module.h>
#include "plugin-support.h"
#include <util/platform.h>

#include <QComboBox>
#include <QDesktopServices>
#include <QDockWidget>
#include <QGridLayout>
#include <QDir>
#include <QFile>
#include <QGestureEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPinchGesture>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <nlohmann/json.hpp>

#include <cmath>
#include <thread>

using nlohmann::json;

namespace rac {

// ---- QR ----------------------------------------------------------------------------------

QrWidget::QrWidget(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(150, 150);
	QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
	sp.setHeightForWidth(true);
	setSizePolicy(sp);
}

void QrWidget::setText(const std::string &text)
{
	text_ = text;
	update();
}

void QrWidget::paintEvent(QPaintEvent *)
{
	if (text_.empty())
		return;
	using qrcodegen::QrCode;
	const QrCode qr = QrCode::encodeText(text_.c_str(), QrCode::Ecc::MEDIUM);
	const int n = qr.getSize();
	const int side = std::min(width(), height());
	const int quiet = 4;
	const double m = double(side) / (n + quiet * 2);
	QPainter p(this);
	const QRectF box((width() - side) / 2.0, (height() - side) / 2.0, side, side);
	// Dark on light, always: inverted codes fail on many scanners (the documented colour exception).
	p.fillRect(box, Qt::white);
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::black);
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++)
			if (qr.getModule(x, y))
				p.drawRect(QRectF(box.left() + (x + quiet) * m, box.top() + (y + quiet) * m, m + 0.5, m + 0.5));
}

// ---- preview -----------------------------------------------------------------------------

PreviewWidget::PreviewWidget(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(140);
	setFocusPolicy(Qt::StrongFocus);
	setAttribute(Qt::WA_AcceptTouchEvents);
	grabGesture(Qt::PinchGesture);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setToolTip("Scroll to zoom. Ctrl+scroll or pinch on a touchpad works too. Keys: + - 1-9");
}

void PreviewWidget::setPose(int rot, bool mirror, bool flip)
{
	if (rot == rot_ && mirror == mirror_ && flip == flip_)
		return;
	rot_ = rot;
	mirror_ = mirror;
	flip_ = flip;
	update();
}

void PreviewWidget::setPrediction(double scale, const QString &label)
{
	scale_ = scale;
	label_ = label;
	update();
}

void PreviewWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const QPalette &pal = palette();
	p.fillRect(rect(), pal.color(QPalette::Base));
	if (img_.isNull()) {
		p.setPen(pal.color(QPalette::PlaceholderText));
		p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, placeholder_);
		return;
	}
	const bool side = rot_ % 180 != 0;
	const double iw = img_.width(), ih = img_.height();
	const double uw = side ? ih : iw, uh = side ? iw : ih;
	const double fit = std::min(width() / uw, height() / uh);
	const QRectF box((width() - uw * fit) / 2, (height() - uh * fit) / 2, uw * fit, uh * fit);
	p.save();
	p.setClipRect(box);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.translate(box.center());
	p.scale(fit * scale_, fit * scale_);
	p.scale(mirror_ ? -1 : 1, flip_ ? -1 : 1);
	p.rotate(rot_);
	p.drawImage(QPointF(-iw / 2, -ih / 2), img_);
	p.restore();

	if (!label_.isEmpty()) {
		QFont f = font();
		f.setBold(true);
		p.setFont(f);
		const QFontMetrics fm(f);
		const int w = fm.horizontalAdvance(label_) + 16, h = fm.height() + 6;
		const QRect pill(int(box.center().x()) - w / 2, int(box.bottom()) - h - 8, w, h);
		QColor bg = pal.color(QPalette::Window);
		bg.setAlpha(200);
		p.setPen(Qt::NoPen);
		p.setBrush(bg);
		p.drawRoundedRect(pill, h / 2.0, h / 2.0);
		p.setPen(pal.color(QPalette::WindowText));
		p.drawText(pill, Qt::AlignCenter, label_);
	}
}

void PreviewWidget::wheelEvent(QWheelEvent *e)
{
	// One wheel notch is 120. A touchpad pinch arrives as Ctrl+wheel with small deltas, so the
	// same curve gives a smooth pinch and x1.12 per notch.
	const double steps = e->angleDelta().y() / 120.0;
	if (steps != 0)
		emit zoomBy(std::pow(1.12, steps));
	e->accept();
}

bool PreviewWidget::event(QEvent *e)
{
	if (e->type() == QEvent::NativeGesture) {
		auto *g = static_cast<QNativeGestureEvent *>(e);
		if (g->gestureType() == Qt::ZoomNativeGesture) {
			emit zoomBy(1.0 + g->value());
			return true;
		}
	}
	if (e->type() == QEvent::Gesture) {
		// Touchscreen pinch: built, but untested (the owner has no touchscreen).
		auto *g = static_cast<QGestureEvent *>(e);
		if (auto *pinch = static_cast<QPinchGesture *>(g->gesture(Qt::PinchGesture))) {
			if (pinch->changeFlags() & QPinchGesture::ScaleFactorChanged)
				emit zoomBy(pinch->scaleFactor());
			return true;
		}
	}
	return QWidget::event(e);
}

void PreviewWidget::keyPressEvent(QKeyEvent *e)
{
	const int k = e->key();
	if (k == Qt::Key_Plus || k == Qt::Key_Equal)
		emit zoomBy(1.12);
	else if (k == Qt::Key_Minus)
		emit zoomBy(1 / 1.12);
	else if (k >= Qt::Key_1 && k <= Qt::Key_9)
		emit presetKey(k - Qt::Key_1);
	else
		QWidget::keyPressEvent(e);
}

// ---- the dock ----------------------------------------------------------------------------

static std::string dockConfigPath()
{
	char *p = obs_module_config_path("dock.json");
	std::string s = p ? p : "dock.json";
	bfree(p);
	return s;
}

static QPointer<PhoneDock> theDock;

void PhoneDock::reveal(bool pair)
{
	PhoneDock *d = theDock;
	if (!d)
		return;
	// The widget OBS wraps it in is the QDockWidget; showing that also ticks Docks > Phone Camera.
	if (auto *dw = qobject_cast<QDockWidget *>(d->parentWidget())) {
		dw->setVisible(true);
		dw->raise();
		dw->activateWindow();
	} else {
		d->show();
	}
	if (pair && !d->addBtn_->isChecked())
		d->addBtn_->setChecked(true);
}

void PhoneDock::introduceOnce()
{
	PhoneDock *d = theDock;
	if (!d || d->introduced_)
		return;
	d->introduced_ = true;
	d->saveConfig();
	reveal(d->phones_.empty());
}

// ---- mic meter -----------------------------------------------------------------------------

MeterWidget::MeterWidget(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setToolTip("The phone's mic level as it arrives. CLIP lights when it's too loud: move the phone back or turn on Auto gain.");
}

void MeterWidget::setLevel(float peak, bool clip)
{
	if (std::fabs(peak - peak_) < 0.004f && clip == clip_)
		return;
	peak_ = peak;
	clip_ = clip;
	update();
}

void MeterWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QPalette &pal = palette();
	const int clipW = 34;
	const QRectF track(0, height() / 2.0 - 3, width() - clipW - 6, 6);
	p.setPen(Qt::NoPen);
	p.setBrush(pal.color(QPalette::Base));
	p.drawRoundedRect(track, 3, 3);
	// OBS's own mixer zones: green to -9 dB, yellow to -3 dB, red above (as fractions of full scale).
	struct Zone {
		double to;
		QColor c;
	};
	const Zone zones[] = {{0.65, QColor(0x25, 0xA2, 0x31)}, {0.85, QColor(0xE5, 0xAF, 0x24)}, {1.0, QColor(0xC0, 0x1C, 0x37)}};
	const double w = track.width() * std::clamp(double(peak_), 0.0, 1.0);
	double from = 0;
	p.save();
	QPainterPath clipPath;
	clipPath.addRoundedRect(track, 3, 3);
	p.setClipPath(clipPath);
	for (const Zone &z : zones) {
		const double x0 = track.width() * from, x1 = std::min(w, track.width() * z.to);
		if (x1 > x0)
			p.fillRect(QRectF(track.left() + x0, track.top(), x1 - x0, track.height()), z.c);
		from = z.to;
	}
	p.restore();
	const QRectF tag(width() - clipW, height() / 2.0 - 7, clipW, 14);
	p.setBrush(clip_ ? QColor(0xC0, 0x1C, 0x37) : pal.color(QPalette::Base));
	p.drawRoundedRect(tag, 3, 3);
	QFont f = font();
	f.setPixelSize(9);
	f.setBold(true);
	p.setFont(f);
	p.setPen(clip_ ? QColor(Qt::white) : pal.color(QPalette::PlaceholderText));
	p.drawText(tag, Qt::AlignCenter, "CLIP");
}

static QToolButton *toggle(const QString &text, const QString &tip)
{
	auto *b = new QToolButton();
	b->setText(text);
	b->setToolTip(tip);
	b->setCheckable(true);
	b->setToolButtonStyle(Qt::ToolButtonTextOnly);
	return b;
}

PhoneDock::PhoneDock(QWidget *parent) : QWidget(parent)
{
	theDock = this;
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	// Phones row: one chip per phone, then "+ Add phone".
	auto *top = new QHBoxLayout();
	chipRow_ = new QHBoxLayout();
	chipRow_->setSpacing(4);
	top->addLayout(chipRow_);
	top->addStretch(1);
	addBtn_ = new QPushButton("+ Add phone");
	addBtn_->setCheckable(true);
	top->addWidget(addBtn_);
	root->addLayout(top);

	status_ = new QLabel();
	status_->setWordWrap(true);
	root->addWidget(status_);

	// Preview, or the Add phone panel in its place.
	stack_ = new QStackedWidget();
	preview_ = new PreviewWidget();
	preview_->setPlaceholder("No phone yet. Click \"+ Add phone\".");
	stack_->addWidget(preview_);

	addPanel_ = new QWidget();
	auto *ap = new QVBoxLayout(addPanel_);
	ap->setContentsMargins(0, 0, 0, 0);
	qrTitle_ = new QLabel();
	qrTitle_->setWordWrap(true);
	qrTitle_->setAlignment(Qt::AlignCenter);
	ap->addWidget(qrTitle_);
	qr_ = new QrWidget();
	ap->addWidget(qr_, 1);
	qrHint_ = new QLabel();
	qrHint_->setWordWrap(true);
	qrHint_->setAlignment(Qt::AlignCenter);
	qrHint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	ap->addWidget(qrHint_);
	auto *apButtons = new QHBoxLayout();
	qrSwap_ = new QPushButton("No app on the phone?");
	auto *usbInstall = new QPushButton("Install app over USB");
	apButtons->addWidget(qrSwap_);
	apButtons->addWidget(usbInstall);
	ap->addLayout(apButtons);
	stack_->addWidget(addPanel_);
	root->addWidget(stack_, 1);

	// Zoom: presets, then the log-scale slider labelled with the phone's own range.
	controls_ = new QWidget();
	auto *cl = new QVBoxLayout(controls_);
	cl->setContentsMargins(0, 0, 0, 0);
	cl->setSpacing(6);
	presetRow_ = new QHBoxLayout();
	presetRow_->setSpacing(4);
	cl->addLayout(presetRow_);
	auto *zr = new QHBoxLayout();
	minLabel_ = new QLabel();
	maxLabel_ = new QLabel();
	zoomLabel_ = new QLabel();
	zoomLabel_->setMinimumWidth(48);
	zoomLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	slider_ = new QSlider(Qt::Horizontal);
	slider_->setRange(0, 1000);
	zr->addWidget(minLabel_);
	zr->addWidget(slider_, 1);
	zr->addWidget(maxLabel_);
	zr->addWidget(zoomLabel_);
	cl->addLayout(zr);

	auto *r1 = new QHBoxLayout();
	camBox_ = new QComboBox();
	camBox_->setToolTip("Camera");
	torchBtn_ = toggle("Torch", "Torch on the phone");
	r1->addWidget(camBox_, 1);
	r1->addWidget(torchBtn_);
	cl->addLayout(r1);

	auto *r2 = new QHBoxLayout();
	modeBox_ = new QComboBox();
	modeBox_->setToolTip("Quality: only what this phone's camera can send");
	codecBox_ = new QComboBox();
	codecBox_->setToolTip("Codec");
	mirrorBtn_ = toggle("Mirror", "Mirror left-right");
	flipBtn_ = toggle("Flip", "Flip upside-down");
	lockBtn_ = toggle("Lock", "Lock orientation: stop following the phone's turning");
	r2->addWidget(modeBox_, 1);
	r2->addWidget(codecBox_);
	r2->addWidget(mirrorBtn_);
	r2->addWidget(flipBtn_);
	r2->addWidget(lockBtn_);
	cl->addLayout(r2);
	root->addWidget(controls_);

	// Mic: a block of its own, because Every scene and Use in other apps are about the OBS
	// source and still work while the phone is offline (docs/mock.html, round 4).
	micPanel_ = new QWidget();
	auto *mg = new QGridLayout(micPanel_);
	mg->setContentsMargins(0, 0, 0, 0);
	mg->setHorizontalSpacing(6);
	mg->setVerticalSpacing(6);
	mg->setColumnStretch(1, 1);
	micBtn_ = toggle("Mic", "Mute or unmute this phone's mic");
	meter_ = new MeterWidget();
	auto *m0 = new QHBoxLayout();
	m0->addWidget(micBtn_);
	m0->addWidget(meter_, 1);
	mg->addWidget(new QLabel("Mic"), 0, 0);
	mg->addLayout(m0, 0, 1);
	inputBox_ = new QComboBox();
	inputBox_->setToolTip("Which of the phone's mics to use. A wired, USB or Bluetooth mic shows up here once it's connected to the phone.");
	mg->addWidget(new QLabel("Input"), 1, 0);
	mg->addWidget(inputBox_, 1, 1);
	nsBtn_ = toggle("Noise", "Noise suppression: cuts steady hiss and hum");
	aecBtn_ = toggle("Echo", "Echo cancellation: removes sound from a speaker near the phone");
	agcBtn_ = toggle("Auto gain", "Automatic gain: evens out loud and quiet, and keeps it from clipping");
	auto *m2 = new QHBoxLayout();
	m2->addWidget(nsBtn_);
	m2->addWidget(aecBtn_);
	m2->addWidget(agcBtn_);
	m2->addStretch(1);
	cleanLabel_ = new QLabel("Clean-up");
	mg->addWidget(cleanLabel_, 2, 0);
	mg->addLayout(m2, 2, 1);
	everyBtn_ = toggle("Every scene", "Hear this mic in every scene, like OBS's own Mic/Aux, so switching scenes or cameras never cuts it");
	micOnlyBtn_ = toggle("Mic only", "Turn the phone's camera off and send only its mic");
	otherBtn_ = toggle("Use in other apps", "Use this mic in Chrome, Discord or Zoom, through the free VB-Audio Virtual Cable");
	auto *m3 = new QHBoxLayout();
	m3->addWidget(everyBtn_);
	m3->addWidget(micOnlyBtn_);
	m3->addWidget(otherBtn_);
	m3->addStretch(1);
	mg->addWidget(new QLabel("Route"), 3, 0);
	mg->addLayout(m3, 3, 1);
	root->addWidget(micPanel_);

	auto *foot = new QHBoxLayout();
	previewBtn_ = toggle("Preview", "Show the preview (turn off for a slim dock)");
	usbBtn_ = new QPushButton("USB");
	usbBtn_->setToolTip("Connect the phone plugged in by USB");
	firewallBtn_ = new QPushButton("Allow phones");
	firewallBtn_->setToolTip("Windows Firewall has no rule for Rely-A-Cam, so phones may be blocked. This adds it.");
	firewallBtn_->hide();
	updateBtn_ = new QPushButton();
	updateBtn_->hide();
	foot->addWidget(previewBtn_);
	foot->addWidget(usbBtn_);
	foot->addWidget(firewallBtn_);
	foot->addStretch(1);
	foot->addWidget(updateBtn_);
	root->addLayout(foot);

	message_ = new QLabel();
	message_->setWordWrap(true);
	message_->hide();
	root->addWidget(message_);

	// Saved preview toggle.
	{
		QFile f(QString::fromStdString(dockConfigPath()));
		if (f.open(QIODevice::ReadOnly)) {
			json j = json::parse(f.readAll().toStdString(), nullptr, false);
			if (j.is_object()) {
				previewOn_ = j.value("preview", true);
				introduced_ = j.value("introduced", false);
			}
		}
	}
	previewBtn_->setChecked(previewOn_);
	setPreviewVisible(previewOn_);

	zoomTimer_ = new QTimer(this);
	zoomTimer_->setSingleShot(true);
	connect(zoomTimer_, &QTimer::timeout, this, &PhoneDock::flushZoom);
	refreshTimer_ = new QTimer(this);
	refreshTimer_->setSingleShot(true);
	connect(refreshTimer_, &QTimer::timeout, this, &PhoneDock::refresh);
	meterTimer_ = new QTimer(this);
	connect(meterTimer_, &QTimer::timeout, this, &PhoneDock::tickMeter);
	meterTimer_->start(50);
	clipAt_.start();
	messageTimer_ = new QTimer(this);
	messageTimer_->setSingleShot(true);
	connect(messageTimer_, &QTimer::timeout, message_, &QLabel::hide);
	lastSent_.start();
	requestedAt_.start();

	// Wiring.
	auto &hub = Hub::get();
	connect(&hub, &Hub::phonesChanged, this, [this] {
		if (!refreshTimer_->isActive())
			refreshTimer_->start(40);
	});
	connect(&hub, &Hub::preview, this, &PhoneDock::onPreview);
	connect(&hub, &Hub::message, this, &PhoneDock::message);
	connect(addBtn_, &QPushButton::toggled, this, &PhoneDock::showAddPanel);
	connect(qrSwap_, &QPushButton::clicked, this, [this] {
		showingAppQr_ = !showingAppQr_;
		showAddPanel(true);
	});
	connect(usbInstall, &QPushButton::clicked, this, &PhoneDock::usbInstall);
	connect(preview_, &PreviewWidget::zoomBy, this, [this](double f) {
		const PhoneView *v = current();
		if (!v)
			return;
		const double base = requested_ > 0 ? requested_ : v->state.zoom;
		requestZoom(base * f);
	});
	connect(preview_, &PreviewWidget::presetKey, this, [this](int i) { Hub::get().preset(selected_, i); });
	connect(slider_, &QSlider::sliderPressed, this, [this] { sliderDragging_ = true; });
	connect(slider_, &QSlider::sliderReleased, this, [this] { sliderDragging_ = false; });
	connect(slider_, &QSlider::valueChanged, this, [this](int pos) {
		if (!slider_->isSliderDown() && !slider_->hasFocus())
			return;
		const PhoneView *v = current();
		const Cam *c = v ? v->caps.cam(v->state.cam) : nullptr;
		if (!c || !c->canZoom())
			return;
		const double lo = std::log(c->min), hi = std::log(c->max);
		requestZoom(std::exp(lo + pos / 1000.0 * (hi - lo)));
	});
	connect(camBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
		Hub::get().sendCmd(selected_, json{{"cam", camBox_->itemData(i).toString().toStdString()}}.dump());
	});
	connect(modeBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
		Hub::get().sendCmd(selected_, json{{"mode", modeBox_->itemData(i).toString().toStdString()}}.dump());
	});
	connect(codecBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
		Hub::get().sendCmd(selected_, json{{"codec", codecBox_->itemData(i).toString().toStdString()}}.dump());
	});
	connect(torchBtn_, &QToolButton::clicked, this, [this](bool on) { Hub::get().sendCmd(selected_, json{{"torch", on}}.dump()); });
	connect(micBtn_, &QToolButton::clicked, this, [this](bool on) { Hub::get().sendCmd(selected_, json{{"mic", on}}.dump()); });
	connect(inputBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
		Hub::get().sendCmd(selected_, json{{"micIn", inputBox_->itemData(i).toString().toStdString()}}.dump());
	});
	connect(nsBtn_, &QToolButton::clicked, this, [this](bool on) { Hub::get().sendCmd(selected_, json{{"ns", on}}.dump()); });
	connect(aecBtn_, &QToolButton::clicked, this, [this](bool on) { Hub::get().sendCmd(selected_, json{{"aec", on}}.dump()); });
	connect(agcBtn_, &QToolButton::clicked, this, [this](bool on) { Hub::get().sendCmd(selected_, json{{"agc", on}}.dump()); });
	connect(micOnlyBtn_, &QToolButton::clicked, this, [this](bool on) { Hub::get().sendCmd(selected_, json{{"micOnly", on}}.dump()); });
	connect(everyBtn_, &QToolButton::clicked, this, &PhoneDock::setEveryScene);
	connect(otherBtn_, &QToolButton::clicked, this, &PhoneDock::setInOtherApps);
	connect(mirrorBtn_, &QToolButton::clicked, this, [this](bool on) { Hub::get().sendCmd(selected_, json{{"mirror", on}}.dump()); });
	connect(flipBtn_, &QToolButton::clicked, this, [this](bool on) { Hub::get().sendCmd(selected_, json{{"flip", on}}.dump()); });
	connect(lockBtn_, &QToolButton::clicked, this, [this](bool on) { Hub::get().sendCmd(selected_, json{{"lock", on}}.dump()); });
	connect(previewBtn_, &QToolButton::toggled, this, &PhoneDock::setPreviewVisible);
	connect(usbBtn_, &QPushButton::clicked, this, &PhoneDock::usbConnect);
	connect(firewallBtn_, &QPushButton::clicked, this, [this] {
		const uint16_t port = Hub::get().port();
		QPointer<PhoneDock> self(this);
		std::thread([self, port] {
			const bool ok = sys::allowThroughFirewall(port, kDiscoveryPort);
			QMetaObject::invokeMethod(qApp, [self, ok] {
				if (!self)
					return;
				self->firewallBtn_->setVisible(!ok);
				self->message(ok ? "Phones are allowed through Windows Firewall now."
						 : "Windows Firewall wasn't changed (the admin prompt was declined?).");
			});
		}).detach();
	});
	connect(updateBtn_, &QPushButton::clicked, this, [this] {
		if (updateUrl_.empty())
			return;
		updateBtn_->setEnabled(false);
		const std::string url = updateUrl_, tag = updateTag_;
		const std::string path = QDir::temp().filePath(QString::fromStdString("RelyACam-Setup-" + tag + ".exe")).toStdString();
		QPointer<PhoneDock> self(this);
		std::thread([self, url, path] {
			const bool ok = sys::download(url, path, [self](int p) {
				QMetaObject::invokeMethod(qApp, [self, p] {
					if (self)
						self->updateBtn_->setText(p >= 0 ? QString("Downloading %1%").arg(p) : "Downloading...");
				});
			});
			const bool ran = ok && sys::runInstaller(path);
			QMetaObject::invokeMethod(qApp, [self, ok, ran] {
				if (!self)
					return;
				self->updateBtn_->setEnabled(true);
				self->updateBtn_->setText("Update");
				self->message(ran  ? "The installer is open. It closes OBS to replace the plugin, then you reopen OBS."
					      : ok ? "Downloaded, but the installer didn't start."
						   : "The update download failed. Check the internet connection.");
			});
		}).detach();
	});

	refresh();
	if (phones_.empty())
		addBtn_->setChecked(true);
	checkFirewall();
	QTimer::singleShot(8000, this, &PhoneDock::checkForUpdate);
}

PhoneDock::~PhoneDock()
{
	Hub::get().endCode();
}

void PhoneDock::showEvent(QShowEvent *e)
{
	Hub::get().setPreviewWanted(previewOn_);
	QWidget::showEvent(e);
}

void PhoneDock::hideEvent(QHideEvent *e)
{
	Hub::get().setPreviewWanted(false);
	QWidget::hideEvent(e);
}

const PhoneView *PhoneDock::current() const
{
	for (auto &p : phones_)
		if (p.device == selected_)
			return &p;
	return nullptr;
}

void PhoneDock::select(const std::string &device)
{
	if (device == selected_)
		return;
	selected_ = device;
	requested_ = 0;
	preview_->clear();
	presetKey_.clear();
	refresh();
}

void PhoneDock::rebuildPhones()
{
	while (QLayoutItem *it = chipRow_->takeAt(0)) {
		delete it->widget();
		delete it;
	}
	for (auto &p : phones_) {
		auto *b = new QToolButton();
		b->setCheckable(true);
		b->setChecked(p.device == selected_);
		b->setText(QString::fromStdString((p.connected ? "● " : "○ ") + p.name));
		b->setToolTip(p.connected ? QString("Connected from %1").arg(QString::fromStdString(p.peer))
					  : "Not connected. Open Rely-A-Cam on this phone; it reconnects by itself.");
		const std::string d = p.device;
		connect(b, &QToolButton::clicked, this, [this, d] { select(d); });
		chipRow_->addWidget(b);
	}
}

void PhoneDock::refresh()
{
	const size_t before = std::count_if(phones_.begin(), phones_.end(), [](const PhoneView &v) { return v.connected; });
	phones_ = Hub::get().phones();
	const size_t now = std::count_if(phones_.begin(), phones_.end(), [](const PhoneView &v) { return v.connected; });

	// A phone that just connected while the QR was up: that was the pairing, so show it.
	if (now > before && addBtn_->isChecked()) {
		for (auto &p : phones_)
			if (p.connected)
				selected_ = p.device;
		addBtn_->setChecked(false);
	}
	if (!current()) {
		selected_.clear();
		for (auto &p : phones_)
			if (p.connected) {
				selected_ = p.device;
				break;
			}
		if (selected_.empty() && !phones_.empty())
			selected_ = phones_.front().device;
	}
	std::string chips = selected_;
	for (auto &p : phones_)
		chips += "|" + p.device + (p.connected ? "+" : "-") + p.name;
	if (chips != chipsKey_) {
		chipsKey_ = chips;
		rebuildPhones();
	}

	const PhoneView *v = current();
	const Cam *c = v ? v->caps.cam(v->state.cam) : nullptr;
	const bool micOnly = v && v->connected && v->state.micOnly;
	// With the camera off, the camera controls can't do anything (DESIGN.md rule 7).
	controls_->setEnabled(v && v->connected && !micOnly);
	usbBtn_->setVisible(true);

	if (!v) {
		status_->setText("Add a phone: install Rely-A-Cam on it, then scan the QR.");
		preview_->setPlaceholder("No phone yet. Click \"+ Add phone\".");
		preview_->clear();
	} else if (!v->connected) {
		status_->setText(QString::fromStdString(v->name) +
				 " is offline. Open Rely-A-Cam on the phone; it reconnects by itself.");
		preview_->setPlaceholder("Waiting for the phone...");
		preview_->clear();
	} else {
		QStringList parts;
		parts << (v->state.live ? "● Live" : "Connected");
		if (!v->state.mode.empty())
			parts << QString::fromStdString(modeLabel(v->state.mode));
		parts << QString::fromStdString(v->state.codec).toUpper();
		parts << QString("%1 Mb/s").arg(v->mbps, 0, 'f', 1);
		parts << QString("%1 ms").arg(v->rttMs);
		parts << (v->usb ? QString("USB") : QString::fromStdString(v->peer));
		if (v->state.temp > 0)
			parts << QString("%1°C").arg(v->state.temp, 0, 'f', 0);
		if (v->state.battery >= 0)
			parts << QString("%1%").arg(v->state.battery);
		status_->setText(parts.join(" · "));
		if (micOnly) {
			preview_->setPlaceholder("Mic only: the phone's camera is off. Turn Mic only off to bring it back.");
			preview_->clear();
		} else {
			preview_->setPlaceholder("Waiting for video...");
		}
	}

	// Camera list.
	camBox_->blockSignals(true);
	camBox_->clear();
	if (v)
		for (auto &cam : v->caps.cams) {
			QString label = QString::fromStdString(cam.label);
			if (cam.canZoom())
				label += QString(" (%1-%2)").arg(QString::fromStdString(zoomLabel(cam.min)), QString::fromStdString(zoomLabel(cam.max)));
			camBox_->addItem(label, QString::fromStdString(cam.id));
			if (cam.id == v->state.cam)
				camBox_->setCurrentIndex(camBox_->count() - 1);
		}
	camBox_->setEnabled(camBox_->count() > 1);
	camBox_->blockSignals(false);

	// Quality: only the modes this camera reported, up to 4K.
	modeBox_->blockSignals(true);
	modeBox_->clear();
	if (c)
		for (auto &m : c->modes) {
			modeBox_->addItem(QString::fromStdString(modeLabel(m)), QString::fromStdString(m));
			if (m == v->state.mode)
				modeBox_->setCurrentIndex(modeBox_->count() - 1);
		}
	modeBox_->blockSignals(false);

	codecBox_->blockSignals(true);
	codecBox_->clear();
	if (v)
		for (auto &k : v->caps.codecs) {
			codecBox_->addItem(QString::fromStdString(k).toUpper(), QString::fromStdString(k));
			if (k == v->state.codec)
				codecBox_->setCurrentIndex(codecBox_->count() - 1);
		}
	codecBox_->setVisible(codecBox_->count() > 1);
	codecBox_->blockSignals(false);

	torchBtn_->setEnabled(c && c->flash);
	torchBtn_->setChecked(v && v->state.torch);
	mirrorBtn_->setChecked(v && v->state.mirror);
	flipBtn_->setChecked(v && v->state.flip);
	lockBtn_->setChecked(v && v->state.lock);

	// Mic.
	const bool live = v && v->connected;
	const bool micOk = live && v->caps.mic;
	micBtn_->setEnabled(micOk);
	micBtn_->setChecked(micOk && v->state.mic);
	micBtn_->setText(!micOk ? "Mic" : v->state.mic ? "On" : "Muted");
	micBtn_->setToolTip(live && !v->caps.mic ? "The phone hasn't allowed Rely-A-Cam to use its microphone. Allow it in the phone's Settings > Apps > Rely-A-Cam > Permissions."
						: "Mute or unmute this phone's mic");
	inputBox_->blockSignals(true);
	inputBox_->clear();
	if (v)
		for (auto &m : v->caps.mics) {
			inputBox_->addItem(QString::fromStdString(m.label), QString::fromStdString(m.id));
			if (m.id == v->state.micIn)
				inputBox_->setCurrentIndex(inputBox_->count() - 1);
		}
	inputBox_->setEnabled(micOk && inputBox_->count() > 0);
	inputBox_->blockSignals(false);
	struct Fx {
		QToolButton *b;
		const char *key;
		bool on;
	};
	const Fx fx[] = {{nsBtn_, "ns", v && v->state.ns}, {aecBtn_, "aec", v && v->state.aec}, {agcBtn_, "agc", v && v->state.agc}};
	bool anyFx = false;
	for (const Fx &f : fx) {
		// Only the effects this phone has (a phone without one can't turn it on).
		const bool has = v && v->caps.hasClean(f.key);
		f.b->setVisible(has);
		f.b->setEnabled(micOk);
		f.b->setChecked(has && f.on);
		anyFx = anyFx || has;
	}
	cleanLabel_->setVisible(anyFx);
	micOnlyBtn_->setEnabled(live);
	micOnlyBtn_->setChecked(micOnly);
	{
		obs_source_t *mic = v ? Hub::get().micSource(v->device) : nullptr;
		everyBtn_->setEnabled(mic != nullptr);
		everyBtn_->setChecked(routing::isEveryScene(mic));
		otherBtn_->setEnabled(mic != nullptr);
		otherBtn_->setChecked(mic && routing::inOtherApps(mic));
		obs_source_release(mic);
	}

	// Presets, rebuilt only when the camera changes.
	const std::string key = c ? v->device + "/" + c->id : "";
	if (key != presetKey_) {
		presetKey_ = key;
		while (QLayoutItem *it = presetRow_->takeAt(0)) {
			delete it->widget();
			delete it;
		}
		if (c) {
			auto stops = presets(c->min, c->max, c->lenses);
			if (stops.empty()) {
				presetRow_->addWidget(new QLabel("This camera can't zoom."));
			}
			for (double z : stops) {
				auto *b = new QPushButton(QString::fromStdString(zoomLabel(z)));
				connect(b, &QPushButton::clicked, this, [this, z] { requestZoom(z); });
				presetRow_->addWidget(b);
			}
		}
		presetRow_->addStretch(1);
	}

	// Zoom value, slider and the dock-only prediction.
	double shown = v ? v->state.zoom : 1;
	double scale = 1;
	if (v && requested_ > 0) {
		const qint64 window = 2 * std::max(v->rttMs, 30) + 350;
		const bool arrived = std::fabs(v->state.zoom - requested_) / requested_ < 0.02;
		if (requestedAt_.elapsed() < window && !arrived) {
			shown = requested_;
			scale = requested_ / std::max(v->state.zoom, 0.01);
			QTimer::singleShot(60, this, [this] {
				if (!refreshTimer_->isActive())
					refreshTimer_->start(0);
			});
		} else {
			requested_ = 0;
		}
	}
	const bool zoomable = c && c->canZoom();
	slider_->setEnabled(zoomable);
	minLabel_->setText(c ? QString::fromStdString(zoomLabel(c->min)) : "");
	maxLabel_->setText(c ? QString::fromStdString(zoomLabel(c->max)) : "");
	zoomLabel_->setText(c ? QString::fromStdString(zoomLabel(shown)) : "");
	if (zoomable && !sliderDragging_ && !slider_->isSliderDown()) {
		const double lo = std::log(c->min), hi = std::log(c->max);
		slider_->blockSignals(true);
		slider_->setValue(int(std::lround((std::log(shown) - lo) / (hi - lo) * 1000)));
		slider_->blockSignals(false);
	}
	if (v && v->connected) {
		preview_->setPose(v->state.rot, v->state.mirror, v->state.flip);
		preview_->setPrediction(scale, c ? QString::fromStdString(zoomLabel(shown)) : QString());
	} else {
		preview_->setPrediction(1, QString());
	}
}

void PhoneDock::tickMeter()
{
	if (!isVisible())
		return;
	const float p = selected_.empty() ? 0.0f : Hub::get().takePeak(selected_);
	// Falls back smoothly, like OBS's meters; rises at once.
	shownPeak_ = std::max(p, shownPeak_ * 0.8f);
	if (p >= 0.99f) {
		clipped_ = true;
		clipAt_.restart();
	}
	if (clipped_ && clipAt_.elapsed() > 1000)
		clipped_ = false;
	meter_->setLevel(shownPeak_, clipped_);
}

void PhoneDock::setEveryScene(bool on)
{
	obs_source_t *mic = Hub::get().micSource(selected_);
	if (!mic)
		return;
	routing::setEveryScene(mic, on);
	const QString name = QString::fromUtf8(obs_source_get_name(mic));
	message(on ? name + " is heard in every scene now, like OBS's own Mic/Aux."
		   : name + " is only in this scene now (it was added to it).");
	obs_source_release(mic);
	refresh();
}

void PhoneDock::setInOtherApps(bool on)
{
	obs_source_t *mic = Hub::get().micSource(selected_);
	if (!mic)
		return;
	switch (routing::setInOtherApps(mic, on)) {
	case routing::OtherApps::NoCable:
		QDesktopServices::openUrl(QUrl(routing::kCableUrl));
		message("Using the phone mic in Chrome, Discord or Zoom needs the free VB-Audio Virtual Cable. Its download page is open: "
			"install it, restart the PC, then turn this on again.");
		break;
	case routing::OtherApps::On:
		message("In Chrome, Discord or Zoom, pick \"CABLE Output\" as the microphone. OBS's monitoring plays into the cable now, "
			"so anything else set to monitor goes there too instead of your headphones. Turn this off to undo it.");
		break;
	case routing::OtherApps::Off:
		break;
	}
	obs_source_release(mic);
	refresh();
}

void PhoneDock::requestZoom(double z)
{
	const PhoneView *v = current();
	const Cam *c = v ? v->caps.cam(v->state.cam) : nullptr;
	if (!c || !c->canZoom())
		return;
	z = std::clamp(z, c->min, c->max);
	requested_ = z;
	requestedAt_.restart();
	pendingSend_ = z;
	// At most ~30 requests a second; the last one always goes out.
	if (lastSent_.elapsed() >= 33)
		flushZoom();
	else if (!zoomTimer_->isActive())
		zoomTimer_->start(int(33 - lastSent_.elapsed()));
	refresh();
}

void PhoneDock::flushZoom()
{
	if (pendingSend_ <= 0)
		return;
	Hub::get().setZoom(selected_, pendingSend_);
	pendingSend_ = 0;
	lastSent_.restart();
}

void PhoneDock::onPreview(const QString &device, const QImage &img)
{
	if (!previewOn_ || device.toStdString() != selected_)
		return;
	preview_->setFrame(img);
}

void PhoneDock::setPreviewVisible(bool on)
{
	previewOn_ = on;
	preview_->setVisible(on);
	stack_->setVisible(on || addBtn_->isChecked());
	Hub::get().setPreviewWanted(on && isVisible());
	saveConfig();
}

void PhoneDock::saveConfig()
{
	QFile f(QString::fromStdString(dockConfigPath()));
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(QByteArray::fromStdString(json{{"preview", previewOn_}, {"introduced", introduced_}}.dump()));
}

void PhoneDock::showAddPanel(bool on)
{
	if (!on) {
		Hub::get().endCode();
		showingAppQr_ = false;
		stack_->setCurrentWidget(preview_);
		stack_->setVisible(previewOn_);
		return;
	}
	stack_->setVisible(true);
	stack_->setCurrentWidget(addPanel_);
	if (showingAppQr_) {
		const std::string url = Hub::get().appUrl();
		qrTitle_->setText("Scan with the phone's own camera to download the app from this PC:");
		qr_->setText(url);
		qrHint_->setText(QString("Or open %1 in the phone's browser.\nAlso on GitHub: github.com/uukjtisa/rely-a-cam")
					 .arg(QString::fromStdString(url)));
		qrSwap_->setText("Back to the pairing QR");
	} else {
		const std::string code = Hub::get().newCode();
		const Pairing p = Hub::get().pairing();
		qrTitle_->setText("Scan with the Rely-A-Cam app on your phone");
		qr_->setText(p.toUri());
		QString hint = QString("Or type the code  %1  in the app.").arg(QString::fromStdString(code));
		if (p.hosts.empty())
			hint = "This PC has no network connection. Connect to Wi-Fi, or use USB.";
		qrHint_->setText(hint);
		qrSwap_->setText("No app on the phone?");
	}
}

void PhoneDock::message(const QString &text)
{
	message_->setText(text);
	message_->show();
	messageTimer_->start(9000);
}

void PhoneDock::checkFirewall()
{
	QPointer<PhoneDock> self(this);
	std::thread([self] {
		const bool ok = sys::firewallRulesPresent();
		QMetaObject::invokeMethod(qApp, [self, ok] {
			if (self)
				self->firewallBtn_->setVisible(!ok);
		});
	}).detach();
}

void PhoneDock::checkForUpdate()
{
	QPointer<PhoneDock> self(this);
	std::thread([self] {
		auto r = sys::latestRelease();
		if (!r || r->installerUrl.empty() || !sys::newerThan(r->tag, PLUGIN_VERSION))
			return;
		const std::string tag = r->tag, url = r->installerUrl;
		QMetaObject::invokeMethod(qApp, [self, tag, url] {
			if (!self)
				return;
			self->updateTag_ = tag;
			self->updateUrl_ = url;
			self->updateBtn_->setText(QString("Update to %1").arg(QString::fromStdString(tag)));
			self->updateBtn_->setToolTip("A newer Rely-A-Cam plugin is out. Remember to update the phone app too.");
			self->updateBtn_->show();
		});
	}).detach();
}

void PhoneDock::usbConnect()
{
	usbBtn_->setEnabled(false);
	message("Looking for a phone over USB...");
	Pairing p = Hub::get().pairing();
	p.hosts = {"127.0.0.1"};
	const std::string uri = p.toUri(), apk = Hub::get().apkPath();
	const uint16_t port = Hub::get().port();
	QPointer<PhoneDock> self(this);
	std::thread([self, uri, apk, port] {
		auto r = sys::connectOverUsb(uri, port, apk);
		QMetaObject::invokeMethod(qApp, [self, r] {
			if (!self)
				return;
			self->usbBtn_->setEnabled(true);
			self->message(QString::fromStdString(r.message));
		});
	}).detach();
}

void PhoneDock::usbInstall()
{
	message("Installing the app over USB...");
	const std::string apk = Hub::get().apkPath();
	QPointer<PhoneDock> self(this);
	std::thread([self, apk] {
		auto r = sys::installOverUsb(apk);
		QMetaObject::invokeMethod(qApp, [self, r] {
			if (self)
				self->message(QString::fromStdString(r.message));
		});
	}).detach();
}

} // namespace rac
