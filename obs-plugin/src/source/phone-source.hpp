// The OBS source: one per phone. Draws the newest picture, turned upright and mirrored or flipped
// as the phone says, and plays the phone's mic. Remembers the phone's camera
// settings in the scene collection, so OBS gives them back on reconnect.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../decode/decoder.hpp"
#include "../net/protocol.hpp"

#include <obs-module.h>

#include <map>
#include <mutex>
#include <string>

namespace rac {

constexpr const char *kSourceId = "rely_a_cam_source";

class PhoneSource {
public:
	PhoneSource(obs_data_t *settings, obs_source_t *source);
	~PhoneSource();

	obs_source_t *obs() const { return source_; }
	std::string device() const;

	// From the network side (any thread).
	void pushPicture(Picture &p);
	void setTransform(int rot, bool mirror, bool flip);
	void remember(const State &s);
	void setConnected(bool on);
	/** OBS's remembered settings as a CMD-shaped JSON object (WELCOME `restore`). */
	std::string restoreJson();

	// OBS callbacks.
	void update(obs_data_t *settings);
	void save(obs_data_t *settings);
	void render(gs_effect_t *effect);
	uint32_t width();
	uint32_t height();

	static void registerSource();

private:
	obs_source_t *source_;
	mutable std::mutex m_;
	std::string device_;
	Picture pending_;
	bool hasPending_ = false;
	gs_texture_t *tex_ = nullptr;
	int tw_ = 0, th_ = 0;
	int rot_ = 0;
	bool mirror_ = false, flip_ = false, connected_ = false;

	// Remembered for reconnects (saved with the scene collection).
	bool remembered_ = false;
	std::string cam_, mode_, codec_;
	std::map<std::string, double> zooms_;
	bool rMirror_ = false, rFlip_ = false, rLock_ = false, rMic_ = true;

	obs_hotkey_id hotkeys_[14] = {};
	void registerHotkeys();
};

} // namespace rac
