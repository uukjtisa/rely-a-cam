// The phone's mic as its own OBS audio source ("Phone Mic - nova 10"), separate from the camera,
// so one phone can be the main mic while another phone's camera is on screen. Where it is heard
// (every scene, or only the scenes it's in) is audio/routing's job. Remembers the phone's mic
// settings (on, input, clean-up) in the scene collection, so OBS gives them back on reconnect.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../net/protocol.hpp"

#include <obs-module.h>

#include <mutex>
#include <string>

namespace rac {

constexpr const char *kMicSourceId = "rely_a_cam_mic";

class PhoneMic {
public:
	PhoneMic(obs_data_t *settings, obs_source_t *source);
	~PhoneMic();

	obs_source_t *obs() const { return source_; }
	std::string device() const;

	/** Keeps the mic part of the phone's state (any thread). */
	void remember(const State &s);
	bool hasMemory() const;
	/** The remembered mic settings as CMD-shaped JSON keys ("mic", "micIn", "ns", "aec", "agc"). */
	std::string restoreJson() const;

	void update(obs_data_t *settings);
	void save(obs_data_t *settings);

	static void registerSource();

private:
	obs_source_t *source_;
	mutable std::mutex m_;
	std::string device_;
	bool remembered_ = false;
	bool mic_ = true, ns_ = false, aec_ = false, agc_ = false;
	std::string micIn_;
};

} // namespace rac
