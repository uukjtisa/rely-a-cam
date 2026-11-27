// Windows-side helpers: the firewall rule (rule 7: name the cause), USB through the bundled adb
// (Phase 3), and the plugin's updater (Phase 4). Blocking calls: run them off the UI thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace rac::sys {

/** True when Rely-A-Cam's inbound rules exist (the installer adds them). */
bool firewallRulesPresent();
/** Adds the rules for OBS on every network profile. Shows Windows' admin prompt. */
bool allowThroughFirewall(uint16_t tcpPort, uint16_t udpPort);
/**
 * Runs [exe] with [args] through Windows' admin prompt, hidden, and waits up to [timeoutMs].
 * False when the prompt was declined or the program couldn't start; [exitCode] gets its exit code.
 */
bool runElevated(const std::string &exe, const std::string &args, unsigned long timeoutMs, unsigned long *exitCode);

struct Result {
	bool ok = false;
	std::string message;
};

/** The bundled adb, or adb on PATH. Empty when neither exists. */
std::string adbPath();
/**
 * USB pairing: finds the phone, installs the app if it is missing (from the copy bundled with
 * the plugin), forwards the phone's port to this PC with `adb reverse`, and opens the pairing
 * link in the app with host 127.0.0.1.
 */
Result connectOverUsb(const std::string &pairingUri, uint16_t port, const std::string &apkPath);
/** Installs (or updates) the app on the USB phone and opens it. */
Result installOverUsb(const std::string &apkPath);

struct Release {
	std::string tag, name, page, installerUrl, apkUrl;
};
/** The newest published release of uukjtisa/rely-a-cam, or nothing (offline, or none yet). */
std::optional<Release> latestRelease();
/** "0.2.0" > "0.1.0", tolerant of a leading v and a -pre suffix. */
bool newerThan(const std::string &tag, const std::string &current);
/** Downloads [url] to [path]; [progress] gets 0..100 (or -1 when the size is unknown). */
bool download(const std::string &url, const std::string &path, const std::function<void(int)> &progress);
/** Runs the downloaded installer; it closes OBS itself when it needs to replace the plugin. */
bool runInstaller(const std::string &path);

} // namespace rac::sys
