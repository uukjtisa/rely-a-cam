// The plugin's network side: one listener, UDP discovery, and one PhoneLink per connected phone.
// Lives on its own thread. The dock and the sources talk to it through the thread-safe calls
// below; it talks back with Qt signals (queued onto the UI thread).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "../decode/decoder.hpp"

#include <obs.h>

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;
class QTimer;
class QThread;

namespace rac {

class PhoneSource;
class PhoneMic;

/** What the dock shows about one phone (a copy; safe to keep). */
struct PhoneView {
	std::string device, name, sourceName, peer;
	bool connected = false;
	bool usb = false;
	Caps caps;
	State state;
	int rttMs = 0;
	double mbps = 0;
};

class PhoneLink;

class Hub : public QObject {
	Q_OBJECT

public:
	static Hub &get();

	void start();
	void stop();

	/** The QR's link, with this PC's addresses as of now. */
	Pairing pairing() const;
	/** Where a phone without the app downloads it from this PC: http://<ip>:<port>/ */
	std::string appUrl() const;
	uint16_t port() const { return port_; }
	/** A fresh 6-character code, answered over UDP until endCode(). Shown as XXX-XXX. */
	std::string newCode();
	void endCode();

	std::vector<PhoneView> phones() const;
	bool hasPhone(const std::string &device) const;

	/** Sends a CMD (a JSON object with any state keys) to a phone. Any thread. */
	void sendCmd(const std::string &device, const std::string &json);
	void nudgeZoom(const std::string &device, double factor);
	void setZoom(const std::string &device, double zoom);
	void preset(const std::string &device, int index);
	void toggle(const std::string &device, const char *key);
	void nextCam(const std::string &device);

	// Sources register themselves; links find their source by the phone's device id.
	void registerSource(PhoneSource *s);
	void unregisterSource(PhoneSource *s);
	void registerMic(PhoneMic *m);
	void unregisterMic(PhoneMic *m);
	void sourceChanged();
	/** The phone's mic source, with a reference the caller releases; null when it has none. */
	obs_source_t *micSource(const std::string &device) const;
	/** The loudest sample (0..1) heard from the phone since the last call; for the dock's meter. */
	float takePeak(const std::string &device);

	// From links (hub or decode threads).
	void deliverPicture(const std::string &device, Picture &p);
	void deliverAudio(const std::string &device, const float *const *planes, int channels, int frames, int rate);
	void deliverState(const std::string &device, const State &s);
	void deliverConnected(const std::string &device, bool on);
	std::string restoreFor(const std::string &device);
	void ensureSource(const std::string &device, const std::string &name);
	void viewChanged(const PhoneView &v, bool gone);
	void emitPreview(const std::string &device, QImage img) { emit preview(QString::fromStdString(device), img); }
	bool previewWanted() const { return previewWanted_.load(); }
	void setPreviewWanted(bool on) { previewWanted_ = on; }

	const std::string &secret() const { return secret_; }
	const std::string &pcId() const { return pcId_; }
	std::string pcName() const;
	std::string apkPath() const;

signals:
	void phonesChanged();
	void preview(QString device, QImage image);
	void message(QString text);

private:
	Hub() = default;
	void loadIdentity();
	void saveIdentity();
	void listen();
	void onNewConnection();
	void onDatagram();
	std::vector<std::string> hosts() const;

	QThread *thread_ = nullptr;
	QTcpServer *server_ = nullptr;
	QUdpSocket *udp_ = nullptr;
	std::string secret_, pcId_;
	uint16_t port_ = kDefaultPort;

	mutable std::mutex m_;
	std::map<std::string, PhoneView> views_;
	std::vector<PhoneSource *> sources_;
	std::vector<PhoneMic *> mics_;
	std::map<std::string, float> peaks_;
	std::map<std::string, PhoneLink *> links_;
	std::string code_;
	std::atomic<bool> previewWanted_{true};

	friend class PhoneLink;
};

/** One phone's connection. Lives on the hub thread; decoding runs on its own worker thread. */
class PhoneLink : public QObject {
	Q_OBJECT

public:
	PhoneLink(QTcpSocket *sock, Hub *hub);
	~PhoneLink() override;
	void sendText(Type t, const std::string &text);
	void close(bool sayBye);
	const std::string &device() const { return device_; }

private:
	void onReadyRead();
	void handle(Frame &f);
	void handleHello(const Frame &f);
	void serveHttp();
	void onTick();
	void publish();
	void workerLoop();

	struct Job {
		Type type;
		std::vector<uint8_t> payload;
	};
	void enqueue(Type t, std::vector<uint8_t> &&p);

	QTcpSocket *sock_;
	Hub *hub_;
	QTimer *timer_;
	Reader reader_;
	bool authed_ = false;
	bool http_ = false;
	bool closing_ = false;
	std::string httpBuf_;
	std::string device_, name_;
	PhoneView view_;
	int64_t lastHeard_ = 0;
	int64_t opened_ = 0;
	uint64_t bytes_ = 0, lastBytes_ = 0;

	std::thread worker_;
	std::mutex qm_;
	std::condition_variable qcv_;
	std::deque<Job> jobs_;
	bool quit_ = false;
	bool waitKey_ = false;
	VideoDecoder vdec_;
	AudioDecoder adec_;
	int audioRate_ = 48000, audioCh_ = 1;
	int64_t lastPreview_ = 0;
};

} // namespace rac
