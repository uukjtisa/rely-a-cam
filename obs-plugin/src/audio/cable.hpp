// VB-Audio Virtual Cable: everything Rely-A-Cam does with it, in one place.
//
// Windows has no way to make an OBS source a microphone, so "Use in other apps" plays the phone
// mic into the cable's playback side ("CABLE Input"); Chrome, Discord and Zoom then record its
// other side ("CABLE Output"). The plugin writes into the cable itself (Writer), so OBS's own
// monitoring device is never borrowed.
//
// While a phone mic is on, CABLE Output can also be Windows' default microphone (all three roles:
// console, multimedia, communications), so apps left on "Default" use the phone. The previous
// default is saved to a file before it's changed and put back when no phone wants it, also after
// a crash (init() puts it back on the next start).
//
// One click installs the cable: download the official pack from vb-audio.com, check it's signed by
// VB-Audio's publisher, run its installer through Windows' admin prompt (Windows still shows its
// own "Install this device software?" box), then put the speakers and mic back as defaults if the
// install moved them. A registry value records that Rely-A-Cam installed it, so the plugin's
// uninstaller offers to remove it only then.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rac::cable {

/** The official driver pack (checked 2026-11-27: 1,324,680 bytes) and its page. */
constexpr const char *kPackUrl = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack46.zip";
constexpr const char *kPageUrl = "https://vb-audio.com/Cable/";

/** "CABLE Input" is there to play into. Any thread. */
bool installed();
/** Rely-A-Cam installed it (the registry marker the uninstaller reads). */
bool installedByUs();

struct Result {
	bool ok = false;
	std::string message;
};

/**
 * Downloads, checks and installs the cable, then restores the default devices. Blocking (a worker
 * thread). [step] gets 0 download, 1 signature, 2 install, 3 defaults, as each starts.
 */
Result install(const std::function<void(int)> &step);
/** Runs the cable's own uninstaller through the admin prompt. Blocking. */
Result remove();

/** Makes CABLE Output Windows' default mic, saving the previous one first. UI thread. */
bool takeDefaultMic();
/** Puts the saved default mic back, if CABLE Output is still the default. UI thread. */
void releaseDefaultMic();
/** CABLE Output is Windows' default mic (console role) right now. UI thread. */
bool isDefaultMic();
/** Drops the saved previous mic without touching Windows: he picked a default himself. UI thread. */
void forgetDefaultMic();

/**
 * Plays audio into CABLE Input, in shared mode: Windows converts the rate and channels. write()
 * runs on OBS's audio thread, straight from the source's audio capture callback, the same pattern
 * OBS's own monitoring uses. A device that goes away marks it failed(); the owner opens a new one.
 */
class Writer {
public:
	Writer();
	~Writer();
	Writer(const Writer &) = delete;
	Writer &operator=(const Writer &) = delete;

	/** Opens CABLE Input for 32-bit float stereo at [rate]. UI thread. */
	bool open(uint32_t rate);
	/**
	 * [planes] are OBS's float planes, before the source's volume ([volume] is applied here, as the
	 * mixer would). Mono is copied to both sides; more than two uses the front pair.
	 */
	void write(const float *const *planes, uint32_t channels, uint32_t frames, float volume, bool silent);
	bool failed() const;

private:
	struct Impl;
	std::unique_ptr<Impl> d_;
};

/** Called once at load: puts back a default mic a crash left switched. UI thread. */
void init();

} // namespace rac::cable
