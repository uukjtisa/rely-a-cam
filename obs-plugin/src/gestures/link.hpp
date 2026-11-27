// Gestures: Rely-A-Cam's link to the Rely-A-Cam-Gestures app (a separate program, so a crash in
// its vision models never takes OBS down). docs/LINK.md in that app's repo is the reference.
//
// A phone with gestures on (the dock's Gestures menu) sends its small preview picture, upright and
// turned the way the source shows it, to the app 15 times a second. The app sends back a layer
// to draw over that phone's source (memes, the detection overlay), the framing crop, sound for
// the Phone Memes source, and actions: phone ones (torch, zoom, snapshot...) are done through the
// hub, OBS ones (scene, record, stream, hotkeys) through OBS's frontend API.
//
// The app is started here when a phone turns gestures on and isn't running yet
// (`--attach --hidden`), and it quits by itself when the last phone turns gestures off, unless
// its window is open.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QImage>
#include <QObject>
#include <QRectF>
#include <QString>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class QLocalSocket;
class QTimer;

namespace rac {

/** What the dock draws over its preview for one phone. */
struct GestureOverlay {
	QImage layer;               // premultiplied BGRA, the source's shape; null when nothing shows
	QRectF crop{0, 0, 1, 1};    // the part of the picture the output shows
	std::vector<std::vector<QPointF>> hands;   // 21 points each, fractions of the picture
	struct Label {
		QString name, state;   // state: "", "armed", "waiting"
		QPointF at;
		float armed = 0;       // 0..1: how long it has been held, of what it needs
		QString confirm;       // the confirm sign's name, for "waiting"
	};
	std::vector<Label> labels;
	std::vector<std::vector<QPointF>> face;   // the face map's lines, fractions of the picture
};

class Gestures : public QObject {
	Q_OBJECT

public:
	static Gestures &get();
	void start();   // UI thread, once OBS has loaded
	void stop();

	// The dock (UI thread).
	bool on(const std::string &device) const;
	void setOn(const std::string &device, bool on);
	/** The app is installed (or a dev build points at it with RACG_EXE). */
	bool installed() const;
	bool connected() const { return connected_; }
	bool ready() const { return ready_; }
	/** One line for the dock's menu: "Gestures ready (GPU)", "Starting the gestures app...". */
	QString status() const;
	bool detectionDock() const { return detDock_; }
	bool detectionOutput() const { return detOut_; }
	void setDetection(bool dock, bool output);
	bool faceDock() const { return faceDock_; }
	bool faceOutput() const { return faceOut_; }
	void setFaceMap(bool dock, bool output);
	void openEditor(const std::string &device);
	GestureOverlay overlay(const std::string &device) const;
	/** An edit or a file from a phone's Library screen (UI thread). */
	void fromPhone(const std::string &device, bool file, const std::string &payload);

	// The hub's decode thread.
	/** Is a picture of this phone wanted now? (Gestures on, linked, and 66 ms since the last.) */
	bool wantsFrame(const std::string &device);
	/** The small decoded picture (BGRA, as decoded) plus the full picture's size. */
	void deliverFrame(const std::string &device, const uint8_t *bgra, int w, int h, int stride, int fullW, int fullH);

signals:
	void changed();

private:
	Gestures() = default;
	void tick();
	void tryConnect();
	void launch();
	void onConnected();
	void onDisconnected();
	void onReadyRead();
	void handle(uint8_t type, const std::string &payload);
	void send(uint8_t type, const std::string &payload);
	void sendPhones();
	void sendPhoneStates();
	void sendObs();
	void onLayer(const std::string &json);
	void onAction(const std::string &json);
	void onAudio(const std::string &payload);
	void loadConfig();
	void saveConfig();
	/** The Library screen's whole state to one phone (or every connected phone when empty). */
	void pushLibrary(const std::string &device = {});
	static void frontendEvent(int event, void *self);

	struct Block;
	struct Pose {
		int rot = 0;
		bool mirror = false, flip = false;
	};

	QLocalSocket *sock_ = nullptr;
	QTimer *timer_ = nullptr;
	std::string buf_;
	bool started_ = false, hello_ = false;
	std::atomic<bool> connected_{false};
	bool ready_ = false, detDock_ = false, detOut_ = false, faceDock_ = false, faceOut_ = false;
	QString device_, error_, appVersion_;
	int64_t launchedAt_ = 0, triedAt_ = 0;
	int launches_ = 0;

	mutable std::mutex m_;
	std::set<std::string> on_;                       // phones with gestures on (saved)
	std::map<std::string, Pose> poses_;              // how each phone's source turns its picture
	std::map<std::string, int64_t> lastFrame_;
	std::map<std::string, std::unique_ptr<Block>> in_, out_;
	std::map<std::string, uint64_t> inSeq_;
	std::map<std::string, GestureOverlay> overlays_;
	std::string lastLib_ = "{}";                     // the app's last LIB, as JSON
};

/** The Phone Memes audio source: meme sounds from the gestures app, in OBS's mixer. */
void registerMemesSource();

} // namespace rac
