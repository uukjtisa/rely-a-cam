// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

using nlohmann::json;

namespace rac {

const char *kCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

static void put32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back(uint8_t(x >> 24));
	v.push_back(uint8_t(x >> 16));
	v.push_back(uint8_t(x >> 8));
	v.push_back(uint8_t(x));
}

std::vector<uint8_t> encode(uint8_t type, const std::vector<uint8_t> &payload)
{
	std::vector<uint8_t> out;
	out.reserve(5 + payload.size());
	out.push_back(type);
	put32(out, uint32_t(payload.size()));
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

std::vector<uint8_t> encodeText(uint8_t type, const std::string &text)
{
	return encode(type, std::vector<uint8_t>(text.begin(), text.end()));
}

std::vector<uint8_t> encodeMedia(uint64_t pts, uint8_t flags, const std::vector<uint8_t> &data)
{
	std::vector<uint8_t> out;
	out.reserve(9 + data.size());
	for (int i = 7; i >= 0; i--)
		out.push_back(uint8_t(pts >> (i * 8)));
	out.push_back(flags);
	out.insert(out.end(), data.begin(), data.end());
	return out;
}

bool parseMedia(const std::vector<uint8_t> &p, Media &out)
{
	if (p.size() < 9)
		return false;
	uint64_t pts = 0;
	for (int i = 0; i < 8; i++)
		pts = (pts << 8) | p[i];
	out.pts = pts;
	out.flags = p[8];
	out.data = p.data() + 9;
	out.size = p.size() - 9;
	return true;
}

bool Reader::feed(const uint8_t *data, size_t size)
{
	if (bad_)
		return false;
	// Compact now and then so the buffer does not grow without bound.
	if (pos_ > 0 && pos_ > buf_.size() / 2) {
		buf_.erase(buf_.begin(), buf_.begin() + long(pos_));
		pos_ = 0;
	}
	buf_.insert(buf_.end(), data, data + size);
	if (buffered() >= 5) {
		const uint8_t *h = buf_.data() + pos_;
		uint32_t len = (uint32_t(h[1]) << 24) | (uint32_t(h[2]) << 16) | (uint32_t(h[3]) << 8) | h[4];
		if (len > kMaxFrame) {
			bad_ = true;
			return false;
		}
	}
	return true;
}

bool Reader::next(Frame &out)
{
	if (bad_ || buffered() < 5)
		return false;
	const uint8_t *h = buf_.data() + pos_;
	uint32_t len = (uint32_t(h[1]) << 24) | (uint32_t(h[2]) << 16) | (uint32_t(h[3]) << 8) | h[4];
	if (len > kMaxFrame) {
		bad_ = true;
		return false;
	}
	if (buffered() < 5 + size_t(len))
		return false;
	out.type = h[0];
	out.payload.assign(h + 5, h + 5 + len);
	pos_ += 5 + len;
	return true;
}

std::string percentEncode(const std::string &s)
{
	std::string out;
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += char(c);
		} else {
			char b[4];
			snprintf(b, sizeof b, "%%%02X", c);
			out += b;
		}
	}
	return out;
}

std::string percentDecode(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
			out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else if (s[i] == '+') {
			out += ' ';
		} else {
			out += s[i];
		}
	}
	return out;
}

std::string Pairing::toUri() const
{
	std::ostringstream o;
	o << "relyacam://pair?v=" << version << "&h=";
	for (size_t i = 0; i < hosts.size(); i++)
		o << (i ? "," : "") << hosts[i];
	o << "&p=" << port << "&k=" << secret << "&id=" << pcId << "&n=" << percentEncode(pcName);
	return o.str();
}

std::string Pairing::toJson() const
{
	json j = {{"v", version}, {"h", hosts}, {"p", port}, {"k", secret}, {"id", pcId}, {"n", pcName}};
	return j.dump();
}

std::optional<Pairing> Pairing::parse(const std::string &uri)
{
	const std::string prefix = "relyacam://pair";
	if (uri.size() < prefix.size())
		return std::nullopt;
	std::string head = uri.substr(0, prefix.size());
	std::transform(head.begin(), head.end(), head.begin(), ::tolower);
	if (head != prefix)
		return std::nullopt;
	auto q = uri.find('?');
	if (q == std::string::npos)
		return std::nullopt;
	std::map<std::string, std::string> kv;
	std::stringstream ss(uri.substr(q + 1));
	std::string part;
	while (std::getline(ss, part, '&')) {
		auto eq = part.find('=');
		if (eq == std::string::npos || eq == 0)
			continue;
		kv[part.substr(0, eq)] = percentDecode(part.substr(eq + 1));
	}
	Pairing p;
	std::stringstream hs(kv["h"]);
	std::string h;
	while (std::getline(hs, h, ','))
		if (!h.empty())
			p.hosts.push_back(h);
	int port = kv.count("p") ? atoi(kv["p"].c_str()) : 0;
	p.secret = kv["k"];
	if (p.hosts.empty() || port <= 0 || port > 65535 || p.secret.size() < 16)
		return std::nullopt;
	p.port = uint16_t(port);
	p.pcId = kv["id"];
	p.pcName = kv["n"];
	p.version = kv.count("v") ? atoi(kv["v"].c_str()) : 1;
	return p;
}

const Cam *Caps::cam(const std::string &id) const
{
	for (auto &c : cams)
		if (c.id == id)
			return &c;
	return nullptr;
}

Caps Caps::fromJson(const std::string &text)
{
	Caps c;
	json j = json::parse(text, nullptr, false);
	if (!j.is_object())
		return c;
	c.device = j.value("device", "");
	c.mic = j.value("mic", false);
	if (j.contains("codecs") && j["codecs"].is_array())
		for (auto &x : j["codecs"])
			if (x.is_string())
				c.codecs.push_back(x.get<std::string>());
	if (j.contains("cams") && j["cams"].is_array()) {
		for (auto &o : j["cams"]) {
			Cam m;
			m.id = o.value("id", "");
			m.facing = o.value("facing", "");
			m.label = o.value("label", "");
			m.min = o.value("min", 1.0);
			m.max = o.value("max", 1.0);
			m.flash = o.value("flash", false);
			if (o.contains("lenses"))
				for (auto &l : o["lenses"])
					if (l.is_number())
						m.lenses.push_back(l.get<double>());
			if (o.contains("modes"))
				for (auto &l : o["modes"])
					if (l.is_string())
						m.modes.push_back(l.get<std::string>());
			c.cams.push_back(m);
		}
	}
	return c;
}

State State::fromJson(const std::string &text)
{
	State s;
	json j = json::parse(text, nullptr, false);
	if (!j.is_object())
		return s;
	s.cam = j.value("cam", "");
	s.zoom = j.value("zoom", 1.0);
	s.torch = j.value("torch", false);
	s.mic = j.value("mic", false);
	s.mode = j.value("mode", "");
	s.codec = j.value("codec", "h264");
	s.rot = j.value("rot", 0);
	s.mirror = j.value("mirror", false);
	s.flip = j.value("flip", false);
	s.lock = j.value("lock", false);
	s.temp = j.value("temp", 0.0);
	s.battery = j.value("battery", -1);
	s.live = j.value("live", false);
	return s;
}

std::vector<double> presets(double min, double max, const std::vector<double> &lenses)
{
	std::vector<double> out;
	if (!(max > min + 1e-3))
		return out;
	for (double l : lenses)
		if (l >= min - 1e-3 && l <= max + 1e-3)
			out.push_back(l);
	bool near2 = false;
	for (double l : out)
		if (std::fabs(l - 2.0) / 2.0 < 0.2)
			near2 = true;
	if (2.0 >= min && 2.0 <= max && !near2)
		out.push_back(2.0);
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return std::fabs(a - b) < 5e-4; }), out.end());
	return out;
}

std::string zoomLabel(double z)
{
	double r = std::round(z * 100.0) / 100.0;
	char b[32];
	if (std::fabs(r - std::round(r)) < 1e-9)
		snprintf(b, sizeof b, "%dx", int(std::round(r)));
	else if (std::fabs(r * 10 - std::round(r * 10)) < 1e-9)
		snprintf(b, sizeof b, "%.1fx", r);
	else
		snprintf(b, sizeof b, "%.2fx", r);
	return b;
}

static bool parseMode(const std::string &m, int &w, int &h, int &fps)
{
	return sscanf(m.c_str(), "%dx%d@%d", &w, &h, &fps) == 3;
}

std::string modeLabel(const std::string &mode)
{
	int w, h, fps;
	if (!parseMode(mode, w, h, fps))
		return mode;
	char b[32];
	if (w >= 3840)
		snprintf(b, sizeof b, "4K %d", fps);
	else
		snprintf(b, sizeof b, "%dp %d", std::min(w, h), fps);
	return b;
}

std::string defaultMode(const std::vector<std::string> &modes)
{
	const long target = 1920L * 1080L;
	std::string best, smallest;
	long bestPx = -1, smallPx = -1;
	int bestFps = 0, smallFps = 0;
	for (auto &m : modes) {
		int w, h, fps;
		if (!parseMode(m, w, h, fps))
			continue;
		long px = long(w) * h;
		if (px <= target && fps <= 30 && (px > bestPx || (px == bestPx && fps > bestFps))) {
			best = m;
			bestPx = px;
			bestFps = fps;
		}
		if (smallPx < 0 || px < smallPx || (px == smallPx && fps < smallFps)) {
			smallest = m;
			smallPx = px;
			smallFps = fps;
		}
	}
	return best.empty() ? smallest : best;
}

std::string normaliseCode(const std::string &raw)
{
	std::string c;
	for (char ch : raw) {
		if (ch == '-' || isspace((unsigned char)ch))
			continue;
		c += char(toupper((unsigned char)ch));
	}
	return c;
}

bool isCode(const std::string &raw)
{
	std::string c = normaliseCode(raw);
	if (c.size() != 6)
		return false;
	for (char ch : c)
		if (!strchr(kCodeAlphabet, ch))
			return false;
	return true;
}

} // namespace rac
