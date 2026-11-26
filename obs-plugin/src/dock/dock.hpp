// The "Phone Camera" dock (DECISIONS.md D-06): one widget that is layouts A, B and C at once.
// A preview toggle gives A (with preview) or B (slim); OBS's own dock floating gives C.
// Colours come from OBS's palette (AGENTS.md rule 5); the QR is the one exception, because a
// scanner needs dark modules on a light background.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../net/hub.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QWidget>

#include <string>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QSlider;
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

/** The dock's live preview: draws frames upright, predicts zoom, and takes wheel/pinch/keys. */
class PreviewWidget : public QWidget {
	Q_OBJECT
public:
	explicit PreviewWidget(QWidget *parent = nullptr);
	void setFrame(const QImage &img) { img_ = img; update(); }
	void setPlaceholder(const QString &t) { placeholder_ = t; update(); }
	void setPose(int rot, bool mirror, bool flip);
	/** Scale to draw the last frame at while a requested zoom is on its way (1 = none). */
	void setPrediction(double scale, const QString &label);
	void clear() { img_ = QImage(); update(); }
	QSize sizeHint() const override { return {320, 180}; }

signals:
	void zoomBy(double factor);
	void presetKey(int index);

protected:
	void paintEvent(QPaintEvent *) override;
	void wheelEvent(QWheelEvent *e) override;
	bool event(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	QImage img_;
	QString placeholder_, label_;
	int rot_ = 0;
	bool mirror_ = false, flip_ = false;
	double scale_ = 1;
};

class PhoneDock : public QWidget {
	Q_OBJECT
public:
	explicit PhoneDock(QWidget *parent = nullptr);
	~PhoneDock() override;

protected:
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	void rebuildPhones();
	void refresh();
	void select(const std::string &device);
	void showAddPanel(bool on);
	void requestZoom(double z);
	void flushZoom();
	void onPreview(const QString &device, const QImage &img);
	void setPreviewVisible(bool on);
	void message(const QString &text);
	void checkFirewall();
	void checkForUpdate();
	void usbConnect();
	void usbInstall();
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
	QWidget *controls_ = nullptr;
	QHBoxLayout *presetRow_ = nullptr;
	QSlider *slider_ = nullptr;
	QLabel *minLabel_ = nullptr, *maxLabel_ = nullptr, *zoomLabel_ = nullptr;
	QComboBox *camBox_ = nullptr, *modeBox_ = nullptr, *codecBox_ = nullptr;
	QToolButton *torchBtn_ = nullptr, *micBtn_ = nullptr, *mirrorBtn_ = nullptr, *flipBtn_ = nullptr, *lockBtn_ = nullptr;
	QToolButton *previewBtn_ = nullptr;
	QPushButton *usbBtn_ = nullptr, *firewallBtn_ = nullptr, *updateBtn_ = nullptr;
	QLabel *message_ = nullptr;
	QTimer *messageTimer_ = nullptr;
	QTimer *zoomTimer_ = nullptr;
	QTimer *refreshTimer_ = nullptr;
	std::string presetKey_;
	std::string chipsKey_;

	// Zoom prediction (D-03): what was asked, when, and what the phone last reported.
	double requested_ = 0;
	QElapsedTimer requestedAt_;
	double pendingSend_ = 0;
	QElapsedTimer lastSent_;
	bool sliderDragging_ = false;
	bool previewOn_ = true;
	std::string updateTag_, updateUrl_;
};

} // namespace rac
