# Rely-A-Cam: rules for agents

Rely-A-Cam turns an Android phone into a camera source for OBS Studio, for free. It has two
halves that speak one protocol:

- **the phone app** (Kotlin, `android/`): it scans the pairing QR, streams the camera, and pinches
  to zoom;
- **the OBS plugin** (C++/Qt, `obs-plugin/`): it adds one source per phone and one "Phone Camera"
  dock that previews, zooms and pairs.

Zoom is synced both ways: pinch on the phone or in the dock, and both follow.

## Layout

```
android/             Gradle, Kotlin
  app/               Compose UI: scan, live, settings
  core/camera/       the ONE Camera2 owner: discovery, zoom, focus, torch, orientation
  core/stream/       MediaCodec encoder, TCP link, frame dropping
  core/protocol/     wire format (pure JVM, unit-tested against protocol/vectors)
  core/qr/           QR decoding (zxing core; from Filet)
obs-plugin/          C++ / CMake, based on the OBS plugin template
  src/source/        the OBS source (video and audio)
  src/dock/          the Qt dock: preview, presets, slider, QR panel
  src/net/           listener, pairing, protocol
  src/decode/        FFmpeg H.264 / HEVC / AAC decoding
  src/sys/           Windows glue: firewall rules, bundled adb, updater (WinHTTP)
  src/third_party/   vendored qrcodegen (MIT)
  tests/             protocol_test.cpp (the vectors, C++ side)
installer/           RelyACam.iss (Inno Setup): the one-click PC installer
protocol/            PROTOCOL.md + golden test vectors both halves must pass
docs/                design notes; mock.html is the approved visual reference
scripts/             package.ps1 (both halves -> release/), release.ps1 (GitHub release)
.github/workflows/   CI: builds both halves and runs both sides of the vectors
DECISIONS.md         every consequential call, why, and what it costs
```

## Ground rules

1. **Nothing about a phone is hardcoded.** Cameras, zoom ranges, lens stops, torch and quality
   modes come only from the capability report the phone sends on connect. Presets are derived
   (lens stops, plus 2x when in range and not within 20% of a lens). A literal like `0.5f`,
   `"1080p"` or a camera ID in feature code is a bug.
2. **Camera discovery goes through Camera2 directly, never CameraX.** CameraX only sees
   `getCameraIdList()`, and some phones hide openable cameras from that list (see DECISIONS.md
   D-04). Discovery also tries unlisted IDs, and hides cameras by rule, never by ID: the physical
   members of an offered logical camera, and sensors whose largest output is under 720p.
3. **The phone owns the zoom value.** The plugin sends requests, and both screens draw what the
   phone reports. The dock preview may *predict* a requested zoom; the program output (what
   viewers see) never shows a prediction.
4. **One owner per job.** Camera: `android/core/camera`. Wire format: `protocol/PROTOCOL.md`,
   implemented once per side (`android/core/protocol`, `obs-plugin/src/net`). A second
   implementation of any of these is a bug, not a shortcut.
5. **The dock uses OBS's theme.** No hardcoded colours in the plugin; read the palette OBS gives
   Qt. The phone app's look follows `docs/mock.html` until `docs/DESIGN.md` exists.
6. **Must never:**
   - need an account or a cloud server (phone and PC talk directly);
   - show ads, track usage, watermark, or lock anything behind a payment;
   - change the owner's OBS scenes, except adding its own source when a phone is added;
   - keep the camera or mic running after streaming stops. Stopping releases both and removes
     the notification.
7. **Failures say why.** A phone that can't reach the PC names the likely cause (Windows
   Firewall on a Public network, different networks). It never just says "failed".

## Testing

- JVM tests and the protocol vectors run on the PC. Device testing is done by the owner on his
  phone and PC.
- **Never run `connected*AndroidTest` against a real phone**: Gradle uninstalls the app and wipes
  its data. Install builds with `adb install -r`.
- Protocol vectors, phone side: `cd android && ./gradlew :core:protocol:test`.
- Protocol vectors, plugin side: `cd obs-plugin && cmake --preset windows-x64 &&
  cmake --build build_x64 --config RelWithDebInfo && build_x64/RelWithDebInfo/rac-protocol-test.exe`
  (or `ctest --test-dir build_x64 -C RelWithDebInfo`).
- A change to the wire format changes `protocol/PROTOCOL.md` and `protocol/vectors/vectors.json`
  first, and both test runs must pass before either side is called done.
- Debug APK: `cd android && ./gradlew :app:assembleDebug`. Release: `scripts/package.ps1`, which
  refuses to run if the plugin and app versions differ.

## Releases

- One tag, `v<version>`, covers both halves. Bump `obs-plugin/buildspec.json` `version` and
  `android/app/build.gradle.kts` `versionName` together, and `versionCode` by one.
- `scripts/release.ps1` packages, tags, pushes the tag and creates a **draft** release with both
  files. Publishing is a human decision (`-Publish`, or the button on GitHub).
- Both updaters read `releases/latest` and look for the asset names `RelyACam-Setup-*.exe` and
  `RelyACam-*.apk`. Renaming an asset breaks every installed copy's updater.
- The release signing key is outside the repo (`keystore.properties`, git-ignored). Android
  refuses an update signed with a different key, so losing it strands every installed app.

## Commits

- Subject: `type(scope): Imperative summary` (feat, fix, refactor, perf, docs, test, build or
  chore), at most 72 characters. The body leads with why, then bullets, then what was verified.
- Author: `Niccc2007 <94959895+uukjtisa@users.noreply.github.com>`. No agent trailers.
- Licences: the plugin is GPL-2.0-or-later (it links libobs); the phone app is GPL-3.0.

## Docs

`SETUP.md` is for people (one-click setup); this file is for agents; `DECISIONS.md` explains why;
`protocol/PROTOCOL.md` defines the wire format.
