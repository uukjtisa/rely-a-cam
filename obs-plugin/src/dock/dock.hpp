// The "Phone Camera" dock, round 5 (docs/mock.html: dockControlsHTML, dockMenu, the cable dialogs).
// Top to bottom: phone chips and "+ Add phone", one status line, the preview (or the Add phone QR
// in its place), the slim zoom ruler, one row of icon tools (mic and its meter, torch, snapshot,
// the menu), then a note line. Everything else lives in one menu: the ⋯ button, or right-click on
// the preview. An option whose parent is off isn't in the menu at all. OBS's own float button
// gives the pop-out window.
//
// Colours come from OBS's palette (docs/DESIGN.md rule 5). The documented exceptions: the QR (dark
// on light, for scanners), OBS's mixer colours (meter zones, CLIP, muted-mic red), the warning
// yellow and the note green.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../gestures/link.hpp"
#include "../net/hub.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QWidget>

#include <functional>
#include <string>
#include <vector>

class QHBoxLayout;
class QLabel;
class QMenu;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace rac {

/** A QR code, drawn with qrcodegen, with its quiet zone. */
class QrWidget : public QWidget {
	Q_OBJECT
public:
	explicit QrWidget(QWidget *parent = nullptr);
	void setText(const std::string &text);
	QSize sizeHint() const override { return {220, 220}; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override { return w; }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	std::string text_;
};

/**
 * The dock's live preview: draws frames upright, predicts zoom, draws the grid, and takes
 * wheel/pinch/keys. Right-click asks for the dock's menu.
 */
class PreviewWidget : public QWidget {
	Q_OBJECT
public:
	explicit PreviewWidget(QWidget *parent = nullptr);
	void setFrame(const QImage &img) { img_ = img; update(); }
	void setPlaceholder(const QString &t);
	void setPose(int rot, bool mirror, bool flip);
	/** Scale to draw the last frame at while a requested zoom is on its way (1 = none), and the pill. */
	void setPrediction(double scale, const QString &pill);
	void setGrid(bool on);
	void setHint(const QString &hint);
	/** What the gestures app draws over this phone: its layer, the framing, the hands. */
	void setGestures(const GestureOverlay &g) { gestures_ = g; }
	void clear() { img_ = QImage(); update(); }
	QSize sizeHint() const override { return {320, 180}; }

signals:
	void zoomBy(double factor);
	void presetKey(int index);
	void menuAt(QPoint global);

protected:
	void paintEvent(QPaintEvent *) override;
	void wheelEvent(QWheelEvent *e) override;
	bool event(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	QImage img_;
	GestureOverlay gestures_;
	QString placeholder_, pill_, hint_;
	int rot_ = 0;
	bool mirror_ = false, flip_ = false, grid_ = false;
	double scale_ = 1;
};

/**
 * The slim zoom ruler (mock `.sruler.dk`): a log-scale track, a labelled tick at each preset and
 * at the maximum, a small tick at each whole step between, and a handle. Drag anywhere to zoom;
 * click a label to jump to it.
 */
class ZoomRuler : public QWidget {
	Q_OBJECT
public:
	explicit ZoomRuler(QWidget *parent = nullptr);
	void setCamera(double min, double max, const std::vector<double> &labels);
	void setValue(double zoom);
	QSize sizeHint() const override { return {240, 30}; }
	QSize minimumSizeHint() const override { return {120, 30}; }

signals:
	void zoomTo(double zoom);
	void zoomBy(double factor);

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;

private:
	double posOf(double z) const;
	double zoomAt(double x) const;
	QRectF track() const;

	double min_ = 1, max_ = 1, value_ = 1;
	std::vector<double> labels_;
	std::vector<std::pair<QRectF, double>> hits_;
	bool dragging_ = false;
};

/**
 * The mic level: OBS's mixer zones (green, yellow, red) and a CLIP light. The zones are one of the
 * places the dock uses fixed colours (docs/DESIGN.md).
 */
class MeterWidget : public QWidget {
	Q_OBJECT
public:
	explicit MeterWidget(QWidget *parent = nullptr);
	void setLevel(float peak, bool clip);
	QSize sizeHint() const override { return {140, 16}; }
	QSize minimumSizeHint() const override { return {60, 16}; }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	float peak_ = 0;
	bool clip_ = false;
};

class PhoneDock : public QWidget {
	Q_OBJECT
public:
	explicit PhoneDock(QWidget *parent = nullptr);
	~PhoneDock() override;

	/**
	 * Shows the dock (OBS keeps a new plugin dock hidden until it's ticked in the Docks menu) and,
	 * with pair = true, opens the Add phone QR. UI thread only. Safe before the dock exists.
	 */
	static void reveal(bool pair);
	/** On the first run ever, reveals the dock with the QR up, so pairing is never a hunt. */
	static void introduceOnce();

protected:
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void changeEvent(QEvent *e) override;

private:
	void rebuildPhones();
	void refresh();
	void select(const std::string &device);
	void showAddPanel(bool on);
	void requestZoom(double z);
	void flushZoom();
	void onPreview(const QString &device, const QImage &img);
	void setPreviewVisible(bool on);
	void saveConfig();
	void message(const QString &text, bool warn = false);
	void checkFirewall();
	void checkForUpdate();
	void usbConnect();
	void usbInstall();
	void tickMeter();
	void styleTools();
	void cmd(const std::string &json);
	void showMenu(QPoint global);
	void fillMenu(QMenu *menu);
	void setEveryScene(bool on);
	void setInOtherApps(bool on);
	void setDefaultMic(bool on);
	void installCable();
	void removeCable();
	const PhoneView *current() const;

	std::string selected_;
	std::vector<PhoneView> phones_;

	QHBoxLayout *chipRow_ = nullptr;
	QPushButton *addBtn_ = nullptr;
	QLabel *status_ = nullptr;
	QStackedWidget *stack_ = nullptr;
	PreviewWidget *preview_ = nullptr;
	QWidget *addPanel_ = nullptr;
	QrWidget *qr_ = nullptr;
	QLabel *qrTitle_ = nullptr;
	QLabel *qrHint_ = nullptr;
	QPushButton *qrSwap_ = nullptr;
	bool showingAppQr_ = false;

	ZoomRuler *ruler_ = nullptr;
	QLabel *noZoom_ = nullptr;
	std::string rulerKey_;

	// The tools row (mock `.dtools`).
	QWidget *tools_ = nullptr;
	QToolButton *micBtn_ = nullptr, *torchBtn_ = nullptr, *snapBtn_ = nullptr, *moreBtn_ = nullptr;
	MeterWidget *meter_ = nullptr;
	QLabel *muted_ = nullptr;
	bool micWarn_ = false;

	QWidget *alerts_ = nullptr;
	QPushButton *firewallBtn_ = nullptr, *updateBtn_ = nullptr;
	QLabel *note_ = nullptr;
	QTimer *noteTimer_ = nullptr;

	QTimer *meterTimer_ = nullptr;
	float shownPeak_ = 0;
	QElapsedTimer clipAt_;
	bool clipped_ = false;
	QTimer *zoomTimer_ = nullptr;
	QTimer *refreshTimer_ = nullptr;
	std::string chipsKey_;
	bool styling_ = false;

	// Zoom prediction: what was asked, when, and what the phone last reported.
	double requested_ = 0;
	QElapsedTimer requestedAt_;
	double pendingSend_ = 0;
	QElapsedTimer lastSent_;
	bool previewOn_ = true;
	bool introduced_ = false;
	std::string updateTag_, updateUrl_;
};

} // namespace rac
