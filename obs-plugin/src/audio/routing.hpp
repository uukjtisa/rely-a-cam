// Where a phone mic is heard, and whether other apps can use it.
//
// "Every scene" puts the mic on one of OBS's global audio channels, the same mechanism as OBS's
// own Desktop Audio and Mic/Aux, so switching scenes or cameras never cuts it. It uses channels
// from 7 up: 0 is the scene and 1-6 belong to Settings > Audio, which rewrites them when that page
// is saved. Off, the mic is an ordinary source in a scene. A mic is in one place or the other,
// never both, so it is never heard twice.
//
// OBS only saves channels 1-6, and drops sources that no scene holds when it loads a scene
// collection, so the every-scene mics are saved and restored here (a frontend save callback).
//
// "Use in other apps" plays the mic into VB-Audio Virtual Cable through OBS's monitoring, so
// Chrome, Discord or Zoom can pick "CABLE Output" as a microphone. Windows has no other way to
// make an OBS source a microphone. OBS's monitoring device is changed only while one is on, and
// given back after.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <obs.h>

namespace rac::routing {

/** The first global channel used for every-scene mics. */
constexpr uint32_t kFirstChannel = 7;
/** Where VB-Audio Virtual Cable is downloaded from (free). */
constexpr const char *kCableUrl = "https://vb-audio.com/Cable/";

/** Any thread. */
bool isEveryScene(obs_source_t *src);
/** Any thread: is some phone mic already heard in every scene? (The first phone is the main mic.) */
bool anyEveryScene();

/** UI thread. The caller holds a reference to [src]. */
void setEveryScene(obs_source_t *src, bool on);
/** UI thread: adds [src] to the current scene unless it's there already. */
void addToCurrentScene(obs_source_t *src);

void registerSaveCallback();
void unregisterSaveCallback();

enum class OtherApps { On, Off, NoCable };
/** UI thread. */
bool inOtherApps(obs_source_t *src);
/** UI thread. NoCable means the cable isn't installed and nothing was changed. */
OtherApps setInOtherApps(obs_source_t *src, bool on);

} // namespace rac::routing
