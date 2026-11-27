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
// "Use in other apps" plays the mic into VB-Audio Virtual Cable with the plugin's own writer
// (cable.hpp), so Chrome, Discord or Zoom can pick "CABLE Output" as a microphone. OBS's
// monitoring device is never touched. "...and be Windows' default mic" makes CABLE Output the
// default while the phone's mic is actually on: on in the app, not muted in OBS, phone connected.
//
// Both are settings on the mic source ("otherApps", "defaultMic"), saved with it. A one-second
// check on the UI thread (start()) makes reality match them: it opens writers, reopens one whose
// device went away, and takes or gives back Windows' default mic on each change. A default he
// picks himself while the phone holds it wins: the phone doesn't take it again until its mic goes
// off and on.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <obs.h>

namespace rac::routing {

/** The first global channel used for every-scene mics. */
constexpr uint32_t kFirstChannel = 7;

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

/** UI thread, after OBS finished loading: starts the check. stop() releases everything. */
void start();
void stop();

/** UI thread. The settings (what the menu ticks), not whether the cable is playing right now. */
bool inOtherApps(obs_source_t *src);
bool isDefaultMic(obs_source_t *src);
/** UI thread. Turning it on also turns the default mic on the first time. The caller checks the cable is installed. */
void setInOtherApps(obs_source_t *src, bool on);
void setDefaultMic(obs_source_t *src, bool on);
/** UI thread: "Use in other apps" off for every phone mic (before VB-Cable is removed). */
void offEverywhere();
/** Is the writer for [src] playing into the cable now? UI thread. */
bool playing(obs_source_t *src);

} // namespace rac::routing
