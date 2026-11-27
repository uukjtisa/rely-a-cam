// Rely-A-Cam wire protocol (protocol/PROTOCOL.md), the plugin's one implementation.
// Pure C++: no OBS, no Qt, so tests/protocol_test.cpp runs it against protocol/vectors.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rac {

constexpr int kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 41127;
constexpr uint16_t kDiscoveryPort = 41126;
constexpr uint32_t kMaxFrame = 16u * 1024u * 1024u;
constexpr uint8_t kFlagKey = 1;
constexpr uint8_t kFlagConfig = 2;

enum class Type : uint8_t {
	Hello = 0x01,
	Welcome = 0x02,
	Caps = 0x03,
	State = 0x04,
	Cmd = 0x05,
	Ping = 0x06,
	Pong = 0x07,
	Bye = 0x08,
	Error = 0x0F,
	VConf = 0x10,
	Video = 0x11,
	AConf = 0x12,
	Audio = 0x13,
};

struct Frame {
	uint8_t type = 0;
	std::vector<uint8_t> payload;
	std::string text() const { return std::string(payload.begin(), payload.end()); }
};

/** `type:u8 length:u32 payload`, big-endian. */
std::vector<uint8_t> encode(uint8_t type, const std::vector<uint8_t> &payload);
std::vector<uint8_t> encodeText(uint8_t type, const std::string &text);

/** A VIDEO/AUDIO payload's header and data. */
struct Media {
	uint64_t pts = 0;
	uint8_t flags = 0;
	const uint8_t *data = nullptr;
	size_t size = 0;
};
std::vector<uint8_t> encodeMedia(uint64_t pts, uint8_t flags, const std::vector<uint8_t> &data);
bool parseMedia(const std::vector<uint8_t> &payload, Media &out);

/**
 * Incremental frame reader: feed bytes as they arrive, take whole frames out.
 * Returns false from feed() when the stream is malformed (oversized frame).
 */
class Reader {
public:
	bool feed(const uint8_t *data, size_t size);
	bool next(Frame &out);
	size_t buffered() const { return buf_.size() - pos_; }

private:
	std::vector<uint8_t> buf_;
	size_t pos_ = 0;
	bool bad_ = false;
};

/** The QR link: relyacam://pair?v=1&h=...&p=...&k=...&id=...&n=... */
struct Pairing {
	std::vector<std::string> hosts;
	uint16_t port = 0;
	std::string secret;
	std::string pcId;
	std::string pcName;
	int version = kProtocolVersion;

	std::string toUri() const;
	std::string toJson() const;
	static std::optional<Pairing> parse(const std::string &uri);
};

/** One camera from the capability report. */
struct Cam {
	std::string id, facing, label;
	double min = 1, max = 1;
	std::vector<double> lenses;
	bool flash = false;
	std::vector<std::string> modes;
	bool canZoom() const { return max > min + 1e-3; }
};

/** An audio input the phone has right now (its id only means something to that phone). */
struct MicInput {
	std::string id, label;
};

struct Caps {
	std::string device;
	std::vector<Cam> cams;
	std::vector<std::string> codecs;
	bool mic = false;
	std::vector<MicInput> mics;
	/** Voice clean-up effects the phone has: "ns", "aec", "agc". */
	std::vector<std::string> clean;
	bool hasClean(const std::string &k) const;
	const Cam *cam(const std::string &id) const;
	static Caps fromJson(const std::string &json);
};

/** The phone's state (it owns every value). */
struct State {
	std::string cam, mode, codec = "h264";
	double zoom = 1;
	bool torch = false, mic = false, mirror = false, flip = false, lock = false, live = false;
	int rot = 0, battery = -1;
	double temp = 0;
	std::string micIn;
	bool ns = false, aec = false, agc = false, micOnly = false;
	static State fromJson(const std::string &json);
};

/** Preset zoom stops from the report: lens stops in range, plus 2x unless a lens is within 20%. */
std::vector<double> presets(double min, double max, const std::vector<double> &lenses);
/** "0.65x", "1x", "2.5x". */
std::string zoomLabel(double z);
/** "1080p 30" / "4K 30" for "1920x1080@30". */
std::string modeLabel(const std::string &mode);
/** 1080p 30 when offered, else the best at or below it, else the smallest. */
std::string defaultMode(const std::vector<std::string> &modes);

/** The discovery code alphabet (no 0/O, 1/I) and helpers. */
extern const char *kCodeAlphabet;
std::string normaliseCode(const std::string &raw);
bool isCode(const std::string &raw);

std::string percentEncode(const std::string &s);
std::string percentDecode(const std::string &s);

} // namespace rac
