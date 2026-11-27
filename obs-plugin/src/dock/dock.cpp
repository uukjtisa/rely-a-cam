// SPDX-License-Identifier: GPL-2.0-or-later
#include "dock.hpp"

#include "../audio/cable.hpp"
#include "../audio/routing.hpp"
#include "../sys/system.hpp"
#include "../third_party/qrcodegen/qrcodegen.hpp"

#include <obs-module.h>
#include "plugin-support.h"
#include <util/platform.h>

#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFrame>
#include <QGestureEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPinchGesture>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <nlohmann/json.hpp>

#include <cmath>
#include <map>
#include <thread>
#include <tuple>

using nlohmann::json;

namespace rac {

// The documented fixed colours (docs/DESIGN.md rule 5): OBS's mixer and warning colours, and the
// mock's note green.
static const QColor kGreen(0x25, 0xA2, 0x31), kYellow(0xE5, 0xAF, 0x24), kRed(0xC0, 0x1C, 0x37);
static const QColor kMutedBg(0x5A, 0x20, 0x30), kNoteGreen(0x5F, 0xD0, 0x6C), kNoZoomBg(0x3A, 0x2F, 0x12);

// The mock's icons (docs/mock.html, IC), drawn in the button's text colour.
static const char *const kIconMic =
	R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="9" y="3" width="6" height="12" rx="3"/><path d="M5 11a7 7 0 0 0 14 0M12 18v3"/></svg>)";
static const char *const kIconMicOff =
	R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="9" y="3" width="6" height="12" rx="3"/><path d="M5 11a7 7 0 0 0 14 0M12 18v3M3 3l18 18"/></svg>)";
static const char *const kIconTorch =
	R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><path d="M7 2h10l-2 6H9z"/><path d="M9 8h6v12a2 2 0 0 1-2 2h-2a2 2 0 0 1-2-2z"/><path d="M12 12v3"/></svg>)";
static const char *const kIconShot =
	R"(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><path d="M4 8h3l2-3h6l2 3h3v11H4z"/><circle cx="12" cy="13" r="3.5"/></svg>)";
static const char *const kIconDots =
	R"(<svg viewBox="0 0 24 24" fill="currentColor"><circle cx="5" cy="12" r="2"/><circle cx="12" cy="12" r="2"/><circle cx="19" cy="12" r="2"/></svg>)";

/** An SVG icon at 15 px in [color], cached (refresh() asks for the same few many times a second). */
static QIcon svgIcon(const char *svg, const QColor &color, qreal dpr)
{
	static std::map<std::tuple<const void *, QRgb, int>, QIcon> cache;
	const auto key = std::make_tuple(static_cast<const void *>(svg), color.rgba(), int(dpr * 100));
	auto it = cache.find(key);
	if (it != cache.end())
		return it->second;
	QByteArray data(svg);
	data.replace("currentColor", color.name().toLatin1());
	QSvgRenderer r(data);
	QPixmap pm(QSize(15, 15) * dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	r.render(&p);
	p.end();
	pm.setDevicePixelRatio(dpr);
	return cache[key] = QIcon(pm);
}

/** The chip's connection light: OBS green when connected, a ring when not. */
static QIcon ledIcon(bool on, const QColor &off, qreal dpr)
{
	QPixmap pm(QSize(8, 8) * dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.scale(dpr, dpr);
	if (on) {
		p.setPen(Qt::NoPen);
		p.setBrush(kGreen);
		p.drawEllipse(QRectF(0.5, 0.5, 7, 7));
	} else {
		p.setPen(QPen(off, 1.2));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(QRectF(1, 1, 6, 6));
	}
	p.end();
	pm.setDevicePixelRatio(dpr);
	return QIcon(pm);
}

/** The rule-of-thirds grid, as on the phone and the output: white at 55% with a soft dark edge. */
static void paintGrid(QPainter &p, const QRectF &box)
{
	p.save();
	p.setRenderHint(QPainter::Antialiasing, false);
	for (int pass = 0; pass < 2; pass++) {
		p.setPen(Qt::NoPen);
		p.setBrush(pass == 0 ? QColor(0, 0, 0, 90) : QColor(255, 255, 255, 140));
		const double t = pass == 0 ? 3 : 1;
		for (int i = 1; i <= 2; i++) {
			const double x = std::round(box.left() + box.width() * i / 3.0), y = std::round(box.top() + box.height() * i / 3.0);
			p.drawRect(QRectF(x - t / 2, box.top(), t, box.height()));
			p.drawRect(QRectF(box.left(), y - t / 2, box.width(), t));
		}
	}
	p.restore();
}

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
	setToolTip("Scroll to zoom. Ctrl+scroll or pinch on a touchpad works too. Keys: + - 1-9. Right-click: menu.");
}

void PreviewWidget::setPlaceholder(const QString &t)
{
	if (t == placeholder_)
		return;
	placeholder_ = t;
	update();
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

void PreviewWidget::setPrediction(double scale, const QString &pill)
{
	if (scale == scale_ && pill == pill_)
		return;
	scale_ = scale;
	pill_ = pill;
	update();
}

void PreviewWidget::setGrid(bool on)
{
	if (on == grid_)
		return;
	grid_ = on;
	update();
}

void PreviewWidget::setHint(const QString &hint)
{
	if (hint == hint_)
		return;
	hint_ = hint;
	update();
}

void PreviewWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QPalette &pal = palette();
	p.fillRect(rect(), pal.color(QPalette::Base));
	if (img_.isNull()) {
		p.setPen(pal.color(QPalette::PlaceholderText));
		p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, placeholder_);
	} else {
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
		if (grid_)
			paintGrid(p, box);
	}

	QFont small = font();
	small.setPixelSize(11);
	p.setFont(small);
	const QFontMetrics fm(small);
	// The zoom pill, top right; a dashed yellow ring while the value shown is a prediction (mock `.zpill.pred`).
	if (!pill_.isEmpty()) {
		QFont f = font();
		f.setPixelSize(12);
		p.setFont(f);
		const QFontMetrics pm(f);
		const int w = pm.horizontalAdvance(pill_) + 14, h = pm.height() + 2;
		const QRectF r(width() - 6 - w, 6, w, h);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 187));
		p.drawRoundedRect(r, h / 2.0, h / 2.0);
		if (scale_ != 1) {
			p.setPen(QPen(kYellow, 1, Qt::DashLine));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(r.adjusted(-2, -2, 2, 2), h / 2.0 + 2, h / 2.0 + 2);
		}
		p.setPen(Qt::white);
		p.drawText(r, Qt::AlignCenter, pill_);
		p.setFont(small);
	}
	// The hint, bottom left (mock `.hint`).
	if (!hint_.isEmpty()) {
		const int w = fm.horizontalAdvance(hint_) + 12, h = fm.height() + 2;
		const QRectF r(6, height() - 6 - h, w, h);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 136));
		p.drawRoundedRect(r, 8, 8);
		p.setPen(QColor(255, 255, 255, 204));
		p.drawText(r, Qt::AlignCenter, hint_);
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

void PreviewWidget::contextMenuEvent(QContextMenuEvent *e)
{
	emit menuAt(e->globalPos());
	e->accept();
}

// ---- zoom ruler ----------------------------------------------------------------------------

ZoomRuler::ZoomRuler(QWidget *parent) : QWidget(parent)
{
	setFixedHeight(30);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setCursor(Qt::PointingHandCursor);
	setToolTip("Drag to zoom, or click a value");
}

void ZoomRuler::setCamera(double min, double max, const std::vector<double> &labels)
{
	min_ = min;
	max_ = std::max(max, min + 1e-3);
	labels_ = labels;
	update();
}

void ZoomRuler::setValue(double zoom)
{
	if (std::fabs(zoom - value_) < 1e-4)
		return;
	value_ = zoom;
	update();
}

QRectF ZoomRuler::track() const
{
	return QRectF(12, 8, width() - 24, 2);
}

double ZoomRuler::posOf(double z) const
{
	const double lo = std::log(min_), hi = std::log(max_);
	return std::clamp((std::log(std::max(z, 1e-3)) - lo) / (hi - lo), 0.0, 1.0);
}

double ZoomRuler::zoomAt(double x) const
{
	const QRectF t = track();
	const double f = std::clamp((x - t.left()) / t.width(), 0.0, 1.0);
	return std::exp(std::log(min_) + f * (std::log(max_) - std::log(min_)));
}

void ZoomRuler::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QPalette &pal = palette();
	const bool on = isEnabled();
	QColor dim = pal.color(QPalette::PlaceholderText);
	QColor text = pal.color(QPalette::Text);
	if (!on) {
		dim.setAlphaF(0.5);
		text.setAlphaF(0.5);
	}
	const QRectF t = track();
	p.setPen(Qt::NoPen);
	p.setBrush(dim);
	p.drawRoundedRect(t, 1, 1);
	auto xOf = [&](double z) { return t.left() + posOf(z) * t.width(); };
	auto labelled = [&](double v) {
		for (double l : labels_)
			if (std::fabs(l - v) < 1e-6)
				return true;
		return false;
	};
	// Small ticks at each whole step that has no label.
	for (double v = std::ceil(min_); v <= std::floor(max_) + 1e-9; v += 1)
		if (!labelled(v))
			p.fillRect(QRectF(std::round(xOf(v)), 6, 1, 6), dim);
	QFont f = font();
	f.setPixelSize(10);
	f.setWeight(QFont::DemiBold);
	p.setFont(f);
	const QFontMetrics fm(f);
	hits_.clear();
	for (double v : labels_) {
		const double x = xOf(v);
		p.fillRect(QRectF(x - 1, 3, 2, 12), text);
		const QString s = QString::fromStdString(zoomLabel(v));
		const int w = fm.horizontalAdvance(s) + 6;
		QRectF r(x - w / 2.0, 16, w, fm.height());
		r.moveLeft(std::clamp(r.left(), 0.0, double(width()) - w));
		p.setPen(text);
		p.drawText(r, Qt::AlignCenter, s);
		p.setPen(Qt::NoPen);
		hits_.push_back({r.adjusted(-2, -2, 2, 2), v});
	}
	// The handle (mock `.hd`): a 14 px disc with a soft shadow.
	const QPointF c(xOf(value_), 9);
	p.setBrush(QColor(0, 0, 0, on ? 110 : 50));
	p.drawEllipse(c + QPointF(0, 1), 7.5, 7.5);
	p.setBrush(text);
	p.drawEllipse(c, 7, 7);
}

void ZoomRuler::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	for (auto &[r, v] : hits_)
		if (r.contains(e->position())) {
			emit zoomTo(v);
			return;
		}
	dragging_ = true;
	emit zoomTo(zoomAt(e->position().x()));
}

void ZoomRuler::mouseMoveEvent(QMouseEvent *e)
{
	if (dragging_)
		emit zoomTo(zoomAt(e->position().x()));
}

void ZoomRuler::mouseReleaseEvent(QMouseEvent *)
{
	dragging_ = false;
}

void ZoomRuler::wheelEvent(QWheelEvent *e)
{
	const double steps = e->angleDelta().y() / 120.0;
	if (steps != 0)
		emit zoomBy(std::pow(1.12, steps));
	e->accept();
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
	const Zone zones[] = {{0.65, kGreen}, {0.85, kYellow}, {1.0, kRed}};
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
	p.setBrush(clip_ ? kRed : pal.color(QPalette::Base));
	p.drawRoundedRect(tag, 3, 3);
	QFont f = font();
	f.setPixelSize(9);
	f.setBold(true);
	p.setFont(f);
	p.setPen(clip_ ? QColor(Qt::white) : pal.color(QPalette::PlaceholderText));
	p.drawText(tag, Qt::AlignCenter, "CLIP");
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

static QToolButton *iconButton(const QString &tip, bool checkable)
{
	auto *b = new QToolButton();
	b->setToolTip(tip);
	b->setCheckable(checkable);
	b->setFixedSize(28, 26);
	b->setIconSize(QSize(15, 15));
	b->setToolButtonStyle(Qt::ToolButtonIconOnly);
	b->setFocusPolicy(Qt::TabFocus);
	return b;
}

/** "1080p30" for the status line (the mock's compact form). */
static QString compactMode(const std::string &mode)
{
	return QString::fromStdString(modeLabel(mode)).remove(' ');
}

/** "hot: try 1080p" (PROTOCOL.md, Heat), worded as on the phone: the rate only when it changes. */
static QString heatHint(const Cam *c, const std::string &mode)
{
	const std::string next = c ? heatSuggestion(c->modes, mode) : std::string();
	if (next.empty())
		return "hot";
	auto fps = [](const std::string &m) {
		const auto at = m.find('@');
		return at == std::string::npos ? std::string() : m.substr(at + 1);
	};
	QString label = QString::fromStdString(modeLabel(next));
	label = fps(next) == fps(mode) ? label.section(' ', 0, 0) : label.remove(' ');
	return "hot: try " + label;
}

PhoneDock::PhoneDock(QWidget *parent) : QWidget(parent)
{
	theDock = this;
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	// Phones: one chip per phone, then "+ Add phone".
	auto *top = new QHBoxLayout();
	top->setSpacing(4);
	chipRow_ = new QHBoxLayout();
	chipRow_->setSpacing(4);
	top->addLayout(chipRow_);
	addBtn_ = new QPushButton("+ Add phone");
	addBtn_->setCheckable(true);
	addBtn_->setToolTip("Show the QR to pair a phone");
	top->addWidget(addBtn_);
	top->addStretch(1);
	root->addLayout(top);

	// One status line (mock `.dline`).
	status_ = new QLabel();
	status_->setWordWrap(true);
	status_->setTextFormat(Qt::RichText);
	{
		QFont f = status_->font();
		f.setPixelSize(12);
		status_->setFont(f);
	}
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
	auto *usbPair = new QPushButton("Connect over USB");
	usbPair->setToolTip("Connect the phone plugged in by USB");
	apButtons->addWidget(qrSwap_);
	apButtons->addWidget(usbInstall);
	apButtons->addWidget(usbPair);
	ap->addLayout(apButtons);
	stack_->addWidget(addPanel_);
	root->addWidget(stack_, 1);

	// The slim ruler, or why there's none.
	ruler_ = new ZoomRuler();
	root->addWidget(ruler_);
	noZoom_ = new QLabel();
	noZoom_->setAutoFillBackground(true);
	{
		QPalette p = noZoom_->palette();
		p.setColor(QPalette::Window, kNoZoomBg);
		p.setColor(QPalette::WindowText, kYellow);
		noZoom_->setPalette(p);
		noZoom_->setContentsMargins(8, 4, 8, 4);
		QFont f = noZoom_->font();
		f.setPixelSize(12);
		noZoom_->setFont(f);
	}
	root->addWidget(noZoom_);

	// The tools row (mock `.dtools`): mic and its meter, torch, snapshot, the menu.
	tools_ = new QWidget();
	auto *tl = new QHBoxLayout(tools_);
	tl->setContentsMargins(0, 0, 0, 0);
	tl->setSpacing(6);
	micBtn_ = iconButton("Mic on: click to mute", true);
	meter_ = new MeterWidget();
	muted_ = new QLabel("mic muted");
	muted_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	torchBtn_ = iconButton("Torch", true);
	snapBtn_ = iconButton("Snapshot", false);
	moreBtn_ = iconButton("Camera, quality, picture, mic, overlays", false);
	tl->addWidget(micBtn_);
	tl->addWidget(meter_, 1);
	tl->addWidget(muted_, 1);
	tl->addWidget(torchBtn_);
	tl->addWidget(snapBtn_);
	tl->addWidget(moreBtn_);
	root->addWidget(tools_);

	// Shown only when something needs doing.
	alerts_ = new QWidget();
	auto *al = new QHBoxLayout(alerts_);
	al->setContentsMargins(0, 0, 0, 0);
	firewallBtn_ = new QPushButton("Allow phones");
	firewallBtn_->setToolTip("Windows Firewall has no rule for Rely-A-Cam, so phones may be blocked. This adds it.");
	firewallBtn_->hide();
	updateBtn_ = new QPushButton();
	updateBtn_->hide();
	al->addWidget(firewallBtn_);
	al->addStretch(1);
	al->addWidget(updateBtn_);
	alerts_->hide();
	root->addWidget(alerts_);

	// The note line (mock `.dnote`): room kept for it, so the dock doesn't jump.
	note_ = new QLabel();
	note_->setWordWrap(true);
	note_->setMinimumHeight(16);
	{
		QFont f = note_->font();
		f.setPixelSize(12);
		note_->setFont(f);
	}
	root->addWidget(note_);

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
	noteTimer_ = new QTimer(this);
	noteTimer_->setSingleShot(true);
	connect(noteTimer_, &QTimer::timeout, note_, &QLabel::clear);
	lastSent_.start();
	requestedAt_.start();

	// Wiring.
	auto &hub = Hub::get();
	connect(&hub, &Hub::phonesChanged, this, [this] {
		if (!refreshTimer_->isActive())
			refreshTimer_->start(40);
	});
	connect(&hub, &Hub::preview, this, &PhoneDock::onPreview);
	connect(&hub, &Hub::message, this, [this](const QString &t) { message(t); });
	connect(addBtn_, &QPushButton::toggled, this, &PhoneDock::showAddPanel);
	connect(qrSwap_, &QPushButton::clicked, this, [this] {
		showingAppQr_ = !showingAppQr_;
		showAddPanel(true);
	});
	connect(usbInstall, &QPushButton::clicked, this, &PhoneDock::usbInstall);
	connect(usbPair, &QPushButton::clicked, this, &PhoneDock::usbConnect);
	auto zoomBy = [this](double f) {
		const PhoneView *v = current();
		if (!v)
			return;
		const double base = requested_ > 0 ? requested_ : v->state.zoom;
		requestZoom(base * f);
	};
	connect(preview_, &PreviewWidget::zoomBy, this, zoomBy);
	connect(ruler_, &ZoomRuler::zoomBy, this, zoomBy);
	connect(ruler_, &ZoomRuler::zoomTo, this, &PhoneDock::requestZoom);
	connect(preview_, &PreviewWidget::presetKey, this, [this](int i) { Hub::get().preset(selected_, i); });
	connect(preview_, &PreviewWidget::menuAt, this, &PhoneDock::showMenu);
	connect(moreBtn_, &QToolButton::clicked, this,
		[this] { showMenu(moreBtn_->mapToGlobal(QPoint(moreBtn_->width(), moreBtn_->height()))); });
	connect(micBtn_, &QToolButton::clicked, this, [this](bool on) { cmd(json{{"mic", on}}.dump()); });
	connect(torchBtn_, &QToolButton::clicked, this, [this](bool on) { cmd(json{{"torch", on}}.dump()); });
	connect(snapBtn_, &QToolButton::clicked, this, [this] {
		cmd(json{{"snap", true}}.dump());
		message("Taking a snapshot...");
	});
	connect(firewallBtn_, &QPushButton::clicked, this, [this] {
		const uint16_t port = Hub::get().port();
		QPointer<PhoneDock> self(this);
		std::thread([self, port] {
			const bool ok = sys::allowThroughFirewall(port, kDiscoveryPort);
			QMetaObject::invokeMethod(qApp, [self, ok] {
				if (!self)
					return;
				self->firewallBtn_->setVisible(!ok);
				self->alerts_->setVisible(!self->firewallBtn_->isHidden() || !self->updateBtn_->isHidden());
				self->message(ok ? "Phones are allowed through Windows Firewall now."
						 : "Windows Firewall wasn't changed (the admin prompt was declined?).",
					      !ok);
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
						   : "The update download failed. Check the internet connection.",
					      !ran);
			});
		}).detach();
	});

	styleTools();
	setPreviewVisible(previewOn_);
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

void PhoneDock::changeEvent(QEvent *e)
{
	// A theme change in OBS: the icons and tool buttons follow its new palette.
	if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange) {
		styleTools();
		chipsKey_.clear();
		if (!refreshTimer_->isActive())
			refreshTimer_->start(0);
	}
	QWidget::changeEvent(e);
}

/** The tool buttons in the theme's colours (mock `.ibtn`): a filled square, Highlight when on. */
void PhoneDock::styleTools()
{
	if (styling_ || !micBtn_)
		return;
	styling_ = true;
	const QPalette &pal = palette();
	const QColor button = pal.color(QPalette::Button), hover = pal.color(QPalette::Mid), hl = pal.color(QPalette::Highlight);
	const QString base = QString("QToolButton{background:%1;border:1px solid %1;border-radius:4px;padding:0}"
				     "QToolButton:hover{border-color:%2}"
				     "QToolButton:checked{background:%3;border-color:%4}")
				     .arg(button.name(), hover.name(), hl.name(), hl.lighter(130).name());
	const QString warn = QString("QToolButton{background:%1;border:1px solid %2;border-radius:4px;padding:0}")
				     .arg(kMutedBg.name(), kRed.name());
	for (QToolButton *b : {torchBtn_, snapBtn_, moreBtn_})
		b->setStyleSheet(base);
	micBtn_->setStyleSheet(micWarn_ ? warn : base);
	const QColor fg = pal.color(QPalette::ButtonText);
	const qreal dpr = devicePixelRatioF();
	torchBtn_->setIcon(svgIcon(kIconTorch, fg, dpr));
	snapBtn_->setIcon(svgIcon(kIconShot, fg, dpr));
	moreBtn_->setIcon(svgIcon(kIconDots, fg, dpr));
	micBtn_->setIcon(svgIcon(micWarn_ ? kIconMicOff : kIconMic, micWarn_ ? QColor(Qt::white) : fg, dpr));
	QPalette mp = muted_->palette();
	mp.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
	muted_->setPalette(mp);
	styling_ = false;
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
	rulerKey_.clear();
	refresh();
}

void PhoneDock::cmd(const std::string &json)
{
	if (!selected_.empty())
		Hub::get().sendCmd(selected_, json);
}

void PhoneDock::rebuildPhones()
{
	while (QLayoutItem *it = chipRow_->takeAt(0)) {
		delete it->widget();
		delete it;
	}
	const QColor off = palette().color(QPalette::PlaceholderText);
	for (auto &p : phones_) {
		auto *b = new QToolButton();
		b->setCheckable(true);
		b->setChecked(p.device == selected_);
		b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		b->setIconSize(QSize(8, 8));
		b->setIcon(ledIcon(p.connected, off, devicePixelRatioF()));
		b->setText(QString::fromStdString(p.name));
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
	const bool live = v && v->connected;
	const bool micOnly = live && v->state.micOnly;
	const State *st = v ? &v->state : nullptr;

	// Status line: link · quality · delay · temperature, with the heat hint and the lock (mock `.dline`).
	if (!v) {
		status_->setText("Add a phone: install Rely-A-Cam on it, then scan the QR.");
		status_->setToolTip(QString());
	} else if (!live) {
		status_->setText(QString::fromStdString(v->name).toHtmlEscaped() +
				 " is offline. Open Rely-A-Cam on the phone; it reconnects by itself.");
		status_->setToolTip(QString());
	} else {
		const QString warmSpan = QString("<span style=\"color:%1\">%2</span>").arg(kYellow.name());
		QStringList parts;
		parts << (QString(v->usb ? "USB" : "Wi-Fi") + (st->live ? "" : " (not live)"));
		if (micOnly)
			parts << "Mic only";
		else if (!st->mode.empty())
			parts << compactMode(st->mode);
		parts << QString("%1 ms").arg(v->rttMs);
		if (st->temp > 0) {
			const QString t = QString("%1 °C").arg(st->temp, 0, 'f', 0);
			parts << (isHot(st->temp) ? warmSpan.arg(t + " " + heatHint(c, st->mode)) : t);
		}
		if (st->aeLock)
			parts << warmSpan.arg("AE/AF locked");
		status_->setText(parts.join(" &#183; "));
		// The details the line leaves out, for whoever wants them.
		QStringList more;
		more << QString::fromStdString(st->codec).toUpper() << QString("%1 Mb/s").arg(v->mbps, 0, 'f', 1)
		     << (v->usb ? QString("over USB") : QString::fromStdString(v->peer));
		if (st->battery >= 0)
			more << QString("battery %1%").arg(st->battery);
		status_->setToolTip(more.join(" · "));
	}

	// Preview: the picture, its grid, the hint.
	if (!v) {
		preview_->setPlaceholder("No phone yet. Click \"+ Add phone\".");
		preview_->clear();
	} else if (!live) {
		preview_->setPlaceholder("Waiting for the phone...");
		preview_->clear();
	} else if (micOnly) {
		preview_->setPlaceholder("Mic only\ncamera off");
		preview_->clear();
	} else {
		preview_->setPlaceholder("Waiting for video...");
	}
	const bool zoomable = c && c->canZoom();
	preview_->setGrid(live && !micOnly && st->grid.dock);
	preview_->setHint(!live ? QString() : zoomable && !micOnly ? "scroll · pinch · right-click: menu" : "right-click: menu");

	// The ruler, rebuilt only when the camera changes; or why there's none.
	ruler_->setVisible(v && !micOnly && zoomable);
	ruler_->setEnabled(live);
	noZoom_->setVisible(v && !micOnly && c && !zoomable);
	if (c && !zoomable)
		noZoom_->setText(QString("This camera can't zoom (%1 only).").arg(QString::fromStdString(zoomLabel(c->min))));
	const std::string key = zoomable ? v->device + "/" + c->id : "";
	if (key != rulerKey_) {
		rulerKey_ = key;
		if (zoomable) {
			std::vector<double> labels = presets(c->min, c->max, c->lenses);
			if (std::none_of(labels.begin(), labels.end(), [&](double l) { return std::fabs(l - c->max) < 1e-6; }))
				labels.push_back(c->max);
			ruler_->setCamera(c->min, c->max, labels);
		}
	}

	// Zoom value and the dock-only prediction.
	double shown = st ? st->zoom : 1;
	double scale = 1;
	if (v && requested_ > 0) {
		const qint64 window = 2 * std::max(v->rttMs, 30) + 350;
		const bool arrived = std::fabs(st->zoom - requested_) / requested_ < 0.02;
		if (requestedAt_.elapsed() < window && !arrived) {
			shown = requested_;
			scale = requested_ / std::max(st->zoom, 0.01);
			QTimer::singleShot(60, this, [this] {
				if (!refreshTimer_->isActive())
					refreshTimer_->start(0);
			});
		} else {
			requested_ = 0;
		}
	}
	if (zoomable)
		ruler_->setValue(shown);
	if (live && !micOnly) {
		preview_->setPose(st->rot, st->mirror, st->flip);
		preview_->setPrediction(scale, c ? QString::fromStdString(zoomLabel(shown)) : QString());
	} else {
		preview_->setPrediction(1, QString());
	}

	// Tools: mic and its meter, torch, snapshot, the menu.
	tools_->setVisible(v != nullptr);
	const bool micOk = live && v->caps.mic;
	const bool warn = micOk && !st->mic;
	micBtn_->setEnabled(micOk);
	micBtn_->setChecked(micOk && st->mic);
	micBtn_->setToolTip(live && !v->caps.mic ? "The phone hasn't allowed Rely-A-Cam to use its microphone. Allow it in the phone's Settings > Apps > Rely-A-Cam > Permissions."
				: warn ? "Mic muted: click to turn on"
				       : "Mic on: click to mute");
	if (warn != micWarn_) {
		micWarn_ = warn;
		styleTools();
	}
	meter_->setVisible(!warn);
	muted_->setVisible(warn);
	torchBtn_->setVisible(c && c->flash && !micOnly);
	torchBtn_->setEnabled(live);
	torchBtn_->setChecked(live && st->torch);
	const bool canSnap = live && !micOnly && !st->snap.empty();
	snapBtn_->setVisible(canSnap);
	if (canSnap)
		snapBtn_->setToolTip(QString("Snapshot: a %1 photo saved on this PC, in Pictures\\Rely-A-Cam").arg(QString::fromStdString(st->snap)));
	moreBtn_->setEnabled(v != nullptr);
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

// ---- the menu (mock dockMenu) ----------------------------------------------------------------

static QAction *addItem(QMenu *m, const QString &text, bool checkable, bool checked, bool enabled, std::function<void()> fn)
{
	QAction *a = m->addAction(text);
	a->setCheckable(checkable);
	a->setChecked(checked);
	a->setEnabled(enabled);
	QObject::connect(a, &QAction::triggered, a, [fn] { fn(); });
	return a;
}

void PhoneDock::showMenu(QPoint global)
{
	if (!current())
		return;
	QMenu menu(this);
	fillMenu(&menu);
	menu.exec(global);
}

void PhoneDock::fillMenu(QMenu *m)
{
	const PhoneView *v = current();
	if (!v)
		return;
	// A copy: the phone's state can change while the menu is open, and every action names the
	// value it sets, worked out from what the menu showed.
	const PhoneView view = *v;
	const State st = view.state;
	const Cam *c = view.caps.cam(st.cam);
	const bool live = view.connected;

	// Camera.
	QMenu *cam = m->addMenu("Camera");
	cam->setEnabled(live && !view.caps.cams.empty());
	for (const auto &k : view.caps.cams) {
		QString label = QString::fromStdString(k.label + ", " + zoomLabel(k.min));
		if (k.canZoom())
			label += "-" + QString::fromStdString(zoomLabel(k.max));
		const std::string id = k.id;
		addItem(cam, label, true, k.id == st.cam, true, [this, id] { cmd(json{{"cam", id}}.dump()); });
	}

	// Quality, then the codec when there's a choice.
	QMenu *q = m->addMenu("Quality");
	q->setEnabled(live && c && !st.micOnly);
	if (c) {
		const std::string def = defaultMode(c->modes);
		for (const auto &mode : c->modes)
			addItem(q, QString::fromStdString(modeLabel(mode) + (mode == def ? " (default)" : "")), true, mode == st.mode, true,
				[this, mode] { cmd(json{{"mode", mode}}.dump()); });
	}
	if (view.caps.codecs.size() > 1) {
		q->addSeparator();
		for (const auto &k : view.caps.codecs)
			addItem(q, QString::fromStdString(k).toUpper(), true, k == st.codec, true, [this, k] { cmd(json{{"codec", k}}.dump()); });
	}

	// Picture.
	QMenu *pic = m->addMenu("Picture");
	pic->setEnabled(live && !st.micOnly);
	addItem(pic, "Mirror", true, st.mirror, true, [this, on = !st.mirror] { cmd(json{{"mirror", on}}.dump()); });
	addItem(pic, "Flip", true, st.flip, true, [this, on = !st.flip] { cmd(json{{"flip", on}}.dump()); });
	addItem(pic, "Lock turn", true, st.lock, true, [this, on = !st.lock] { cmd(json{{"lock", on}}.dump()); });
	pic->addSeparator();
	QAction *ae = addItem(pic, "Lock focus + exposure", true, st.aeLock, c && c->aeLock,
			      [this, on = !st.aeLock] { cmd(json{{"aeLock", on}}.dump()); });
	if (!(c && c->aeLock))
		ae->setToolTip("This camera can't lock focus and exposure.");
	if (c && !c->wb.empty()) {
		QMenu *wb = pic->addMenu("White balance");
		for (const auto &k : c->wb)
			addItem(wb, QString::fromStdString(whiteBalanceLabel(k)), true, k == st.wb, true, [this, k] { cmd(json{{"wb", k}}.dump()); });
	}
	pic->addSeparator();
	addItem(pic, st.snap.empty() ? "Snapshot to PC (not with this camera)" : "Snapshot to PC", false, false, !st.snap.empty(), [this] {
		cmd(json{{"snap", true}}.dump());
		message("Taking a snapshot...");
	});

	// Mic. Every scene and Use in other apps are about the OBS source, so they stay while the
	// phone is offline; while it's muted they're hidden (DESIGN.md rule 7).
	QMenu *mic = m->addMenu("Mic");
	obs_source_t *src = Hub::get().micSource(view.device);
	const bool micOk = live && view.caps.mic;
	addItem(mic, live && !view.caps.mic ? "Mic on (not allowed on the phone)" : "Mic on", true, live && st.mic, micOk,
		[this, on = !st.mic] { cmd(json{{"mic", on}}.dump()); });
	if (!live || st.mic) {
		if (live && !view.caps.mics.empty()) {
			QMenu *in = mic->addMenu("Input");
			for (const auto &mi : view.caps.mics) {
				const std::string id = mi.id;
				addItem(in, QString::fromStdString(mi.label), true, mi.id == st.micIn, true,
					[this, id] { cmd(json{{"micIn", id}}.dump()); });
			}
		}
		struct Fx {
			const char *key, *label;
			bool on;
		};
		const Fx fx[] = {{"ns", "Noise", st.ns}, {"aec", "Echo", st.aec}, {"agc", "Auto gain", st.agc}};
		if (live && std::any_of(std::begin(fx), std::end(fx), [&](const Fx &f) { return view.caps.hasClean(f.key); })) {
			QMenu *clean = mic->addMenu("Clean-up");
			for (const Fx &f : fx)
				if (view.caps.hasClean(f.key)) {
					const std::string k = f.key;
					addItem(clean, f.label, true, f.on, true, [this, k, on = !f.on] { cmd(json{{k, on}}.dump()); });
				}
		}
		const bool every = routing::isEveryScene(src);
		addItem(mic, "Every scene", true, every, src != nullptr, [this, on = !every] { setEveryScene(on); });
		mic->addSeparator();
		if (cable::installed()) {
			const bool apps = src && routing::inOtherApps(src);
			addItem(mic, "Use in other apps", true, apps, src != nullptr, [this, on = !apps] { setInOtherApps(on); });
			if (apps) {
				const bool def = routing::isDefaultMic(src);
				addItem(mic, "...and be Windows' default mic", true, def, true, [this, on = !def] { setDefaultMic(on); });
			}
			addItem(mic, "Remove VB-Cable...", false, false, true, [this] { removeCable(); });
		} else {
			addItem(mic, "Use in other apps...", false, false, src != nullptr, [this] { installCable(); });
		}
		mic->addSeparator();
	}
	addItem(mic, "Mic only (camera off)", true, st.micOnly, live, [this, on = !st.micOnly] { cmd(json{{"micOnly", on}}.dump()); });
	obs_source_release(src);

	// Overlays. The phone owns the grid setting, so both screens agree (PROTOCOL.md, STATE `grid`).
	QMenu *ov = m->addMenu("Overlays");
	ov->setEnabled(live);
	QMenu *grid = ov->addMenu("Grid");
	const std::pair<const char *, const char *> places[] = {{"dock", "Dock"}, {"phone", "Phone"}, {"out", "OBS output"}};
	const bool gridOn[] = {st.grid.dock, st.grid.phone, st.grid.out};
	for (int i = 0; i < 3; i++) {
		const std::string k = places[i].first;
		addItem(grid, places[i].second, true, gridOn[i], true, [this, k, on = !gridOn[i]] { cmd(json{{"grid", {{k, on}}}}.dump()); });
	}

	m->addSeparator();
	addItem(m, "Show preview", true, previewOn_, true, [this] { setPreviewVisible(!previewOn_); });
	addItem(m, "Connect a phone over USB", false, false, true, [this] { usbConnect(); });
}

// ---- mic routing ---------------------------------------------------------------------------

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
	routing::setInOtherApps(mic, on);
	if (on)
		message(routing::isDefaultMic(mic)
				? "Apps left on \"Default\" use this phone's mic while it's on. Apps with a mic list can pick \"CABLE Output\"."
				: "In Chrome, Discord or Zoom, pick \"CABLE Output\" as the microphone.");
	else
		message("This phone's mic is no longer in other apps.");
	obs_source_release(mic);
	refresh();
}

void PhoneDock::setDefaultMic(bool on)
{
	obs_source_t *mic = Hub::get().micSource(selected_);
	if (!mic)
		return;
	routing::setDefaultMic(mic, on);
	message(on ? "This phone's mic is Windows' default mic while it's on." : "Windows' default mic is left alone now.");
	obs_source_release(mic);
}

/** The install dialog (mock cableInstallDlg / runCable): five steps that tick as they finish. */
void PhoneDock::installCable()
{
	const PhoneView *v = current();
	if (!v)
		return;
	const std::string device = v->device;
	const QString name = QString::fromStdString(v->name).toHtmlEscaped();

	auto *dlg = new QDialog(window());
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle("Rely-A-Cam");
	dlg->setMinimumWidth(460);
	auto *lay = new QVBoxLayout(dlg);
	lay->setSpacing(8);
	auto *title = new QLabel(QString("<b>Use %1's mic in other apps?</b>").arg(name));
	auto *body = new QLabel("Chrome, Discord and Zoom can only use an OBS mic through a virtual cable. Rely-A-Cam sets it up:");
	body->setWordWrap(true);
	body->setTextFormat(Qt::RichText);
	auto *box = new QFrame();
	box->setFrameShape(QFrame::StyledPanel);
	auto *bl = new QVBoxLayout(box);
	bl->setSpacing(4);
	const QStringList texts = {
		"Download VB-Audio Virtual Cable from vb-audio.com (1.3 MB, donationware)",
		"Check it's signed by VB-Audio",
		"Install it: Windows asks for admin permission, then shows its own \"Install this device software?\" box",
		"Put your speakers and mic back as Windows' defaults if the install moved them",
		"Turn on Use in other apps",
	};
	std::vector<QLabel *> steps;
	for (const QString &t : texts) {
		auto *l = new QLabel(t.toHtmlEscaped());
		l->setWordWrap(true);
		l->setTextFormat(Qt::RichText);
		bl->addWidget(l);
		steps.push_back(l);
	}
	auto *foot = new QLabel();
	foot->setWordWrap(true);
	foot->hide();
	auto *row = new QHBoxLayout();
	auto *working = new QLabel("Working...");
	working->hide();
	auto *go = new QPushButton("Download and install");
	go->setDefault(true);
	auto *cancel = new QPushButton("Cancel");
	auto *ok = new QPushButton("OK");
	ok->hide();
	row->addWidget(working);
	row->addStretch(1);
	row->addWidget(go);
	row->addWidget(cancel);
	row->addWidget(ok);
	lay->addWidget(title);
	lay->addWidget(body);
	lay->addWidget(box);
	lay->addWidget(foot);
	lay->addLayout(row);

	const QString muted = palette().color(QPalette::PlaceholderText).name();
	const QString text = palette().color(QPalette::WindowText).name();
	auto mark = [steps, texts, muted, text](int now) {
		for (int k = 0; k < int(steps.size()); k++) {
			const QString t = texts[k].toHtmlEscaped();
			if (k < now)
				steps[k]->setText(QString("<span style=\"color:%1\">&#10003;&nbsp; %2</span>").arg(kNoteGreen.name(), t));
			else if (k == now)
				steps[k]->setText(QString("<span style=\"color:%1\">%2 ...</span>").arg(text, t));
			else
				steps[k]->setText(QString("<span style=\"color:%1\">%2</span>").arg(muted, t));
		}
	};
	mark(-1);
	connect(cancel, &QPushButton::clicked, dlg, &QDialog::reject);
	connect(ok, &QPushButton::clicked, dlg, &QDialog::accept);
	QPointer<PhoneDock> self(this);
	QPointer<QDialog> d(dlg);
	connect(go, &QPushButton::clicked, dlg, [=] {
		go->hide();
		cancel->hide();
		working->show();
		mark(0);
		std::thread([=] {
			const cable::Result r = cable::install([=](int s) {
				QMetaObject::invokeMethod(qApp, [=] {
					if (d)
						mark(s);
				});
			});
			QMetaObject::invokeMethod(qApp, [=] {
				if (!d)
					return;
				working->hide();
				ok->show();
				if (!r.ok) {
					foot->setText(QString("<span style=\"color:%1\">%2</span>")
							      .arg(kYellow.name(), QString::fromStdString(r.message).toHtmlEscaped()));
					foot->setTextFormat(Qt::RichText);
					foot->show();
					return;
				}
				mark(4);
				if (self) {
					obs_source_t *mic = Hub::get().micSource(device);
					if (mic)
						routing::setInOtherApps(mic, true);
					obs_source_release(mic);
					self->refresh();
				}
				mark(5);
				title->setText("<b>Done</b>");
				body->setText(QString("%1's mic is now Windows' default mic while it's on, so apps left on \"Default\" use it. "
						      "Apps with a mic list can pick <b>CABLE Output</b>.")
						      .arg(name));
				box->hide();
				foot->setText(QString("<span style=\"color:%1\">Mute it on the phone or in OBS and Windows gets its old default mic back at once.</span>")
						      .arg(muted));
				foot->setTextFormat(Qt::RichText);
				foot->show();
			});
		}).detach();
	});
	dlg->show();
}

/** The removal dialog (mock cableRemoveDlg). */
void PhoneDock::removeCable()
{
	QMessageBox box(QMessageBox::NoIcon, "Rely-A-Cam", "<b>Remove VB-Audio Virtual Cable?</b>", QMessageBox::NoButton, window());
	box.setInformativeText("Apps using \"CABLE Output\" as their mic lose it, and Use in other apps turns off for every phone. "
			       "Windows asks for admin permission; a restart may be needed to finish.");
	QPushButton *rm = box.addButton("Remove", QMessageBox::AcceptRole);
	box.addButton(QMessageBox::Cancel);
	box.setDefaultButton(rm);
	box.exec();
	if (box.clickedButton() != rm)
		return;
	routing::offEverywhere();
	message("Removing VB-Cable...");
	QPointer<PhoneDock> self(this);
	std::thread([self] {
		const cable::Result r = cable::remove();
		QMetaObject::invokeMethod(qApp, [self, r] {
			if (self) {
				self->message(QString::fromStdString(r.message), !r.ok);
				self->refresh();
			}
		});
	}).detach();
}

// ---- zoom, preview, QR, notes --------------------------------------------------------------

void PhoneDock::requestZoom(double z)
{
	const PhoneView *v = current();
	const Cam *c = v ? v->caps.cam(v->state.cam) : nullptr;
	if (!c || !c->canZoom() || !v->connected)
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
	const PhoneView *v = current();
	if (v && v->state.micOnly)
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

void PhoneDock::message(const QString &text, bool warn)
{
	QPalette p = note_->palette();
	p.setColor(QPalette::WindowText, warn ? kYellow : kNoteGreen);
	note_->setPalette(p);
	note_->setText(text);
	noteTimer_->start(9000);
}

void PhoneDock::checkFirewall()
{
	QPointer<PhoneDock> self(this);
	std::thread([self] {
		const bool ok = sys::firewallRulesPresent();
		QMetaObject::invokeMethod(qApp, [self, ok] {
			if (!self)
				return;
			self->firewallBtn_->setVisible(!ok);
			self->alerts_->setVisible(!ok || !self->updateBtn_->isHidden());
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
			self->alerts_->show();
		});
	}).detach();
}

void PhoneDock::usbConnect()
{
	message("Looking for a phone over USB...");
	Pairing p = Hub::get().pairing();
	p.hosts = {"127.0.0.1"};
	const std::string uri = p.toUri(), apk = Hub::get().apkPath();
	const uint16_t port = Hub::get().port();
	QPointer<PhoneDock> self(this);
	std::thread([self, uri, apk, port] {
		auto r = sys::connectOverUsb(uri, port, apk);
		QMetaObject::invokeMethod(qApp, [self, r] {
			if (self)
				self->message(QString::fromStdString(r.message), !r.ok);
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
				self->message(QString::fromStdString(r.message), !r.ok);
		});
	}).detach();
}

} // namespace rac
