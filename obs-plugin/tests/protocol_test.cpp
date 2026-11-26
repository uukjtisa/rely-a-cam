// protocol/vectors/vectors.json against the plugin's protocol code. Same cases as the phone's
// VectorsTest.kt: if either side drifts from PROTOCOL.md, one of the two fails.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/protocol.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using nlohmann::json;
using namespace rac;

static int failures = 0;
#define CHECK(cond, what)                                              \
	do {                                                           \
		if (!(cond)) {                                         \
			std::printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); \
			failures++;                                    \
		}                                                      \
	} while (0)

static std::vector<uint8_t> hex(const std::string &s)
{
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < s.size(); i += 2)
		out.push_back(uint8_t(std::stoi(s.substr(i, 2), nullptr, 16)));
	return out;
}

int main()
{
	std::ifstream in(RAC_VECTORS);
	if (!in) {
		std::printf("FAIL cannot open %s\n", RAC_VECTORS);
		return 2;
	}
	std::stringstream ss;
	ss << in.rdbuf();
	json v = json::parse(ss.str());

	int cases = 0;
	for (auto &c : v["frames"]) {
		cases++;
		std::vector<uint8_t> payload;
		if (c.contains("payload_text")) {
			std::string t = c["payload_text"];
			payload.assign(t.begin(), t.end());
		} else if (c.contains("media")) {
			payload = encodeMedia(c["media"]["pts"].get<uint64_t>(), uint8_t(c["media"]["flags"].get<int>()),
					      hex(c["media"]["data_hex"]));
		} else {
			payload = hex(c["payload_hex"]);
		}
		auto bytes = encode(uint8_t(c["type"].get<int>()), payload);
		const std::string name = c["name"];
		CHECK(bytes == hex(c["hex"]), name.c_str());
		// Fed one byte at a time, the reader must still find exactly this frame.
		Reader r;
		Frame f;
		int got = 0;
		for (uint8_t b : bytes) {
			r.feed(&b, 1);
			while (r.next(f))
				got++;
		}
		CHECK(got == 1 && f.payload == payload && f.type == c["type"].get<int>(), (name + " (reader)").c_str());
	}
	for (auto &c : v["pairing"]) {
		cases++;
		auto p = Pairing::parse(c["uri"]);
		const std::string uri = c["uri"];
		if (c.value("invalid", false)) {
			CHECK(!p, uri.c_str());
			continue;
		}
		CHECK(p.has_value(), uri.c_str());
		if (!p)
			continue;
		CHECK(p->hosts == c["hosts"].get<std::vector<std::string>>(), "hosts");
		CHECK(p->port == c["port"].get<int>(), "port");
		CHECK(p->secret == c["secret"].get<std::string>(), "secret");
		CHECK(p->pcId == c["id"].get<std::string>(), "id");
		CHECK(p->pcName == c["name"].get<std::string>(), "name");
		auto back = Pairing::parse(p->toUri());
		CHECK(back && back->pcName == p->pcName && back->hosts == p->hosts, "round trip");
	}
	for (auto &c : v["presets"]) {
		cases++;
		auto got = presets(c["min"], c["max"], c["lenses"].get<std::vector<double>>());
		auto want = c["expect"].get<std::vector<double>>();
		bool same = got.size() == want.size();
		for (size_t i = 0; same && i < got.size(); i++)
			same = std::fabs(got[i] - want[i]) < 1e-6;
		CHECK(same, c["name"].get<std::string>().c_str());
	}
	for (auto &c : v["defaultMode"]) {
		cases++;
		CHECK(defaultMode(c["modes"].get<std::vector<std::string>>()) == c["expect"].get<std::string>(), "defaultMode");
	}
	for (auto &c : v["codes"]) {
		cases++;
		const std::string raw = c["raw"];
		CHECK(isCode(raw) == c["ok"].get<bool>(), raw.c_str());
		if (c.contains("normal"))
			CHECK(normaliseCode(raw) == c["normal"].get<std::string>(), raw.c_str());
	}
	std::printf("%d cases, %d failures\n", cases, failures);
	return failures ? 1 : 0;
}
