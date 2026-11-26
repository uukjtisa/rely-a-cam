# Rely-A-Cam: decisions

A learning log. Each entry gives the call, the reasoning, what established products do at the
same fork, what the call costs, and where to read more. `D-nn` is a call Claire made, or
recommended and the owner accepted. `O-nn` is an owner's call that overruled her, with an honest
verdict. A reversed decision keeps its entry, and the reversal gets a new one.

---

## D-01 The PC side is an OBS plugin, not a standalone desktop app (2026-11-26)

**The call:** the PC half is a C++ OBS plugin (a source plus a dock), not a Kotlin desktop app.

**Why:**
- The target is OBS; the owner's first question was what OBS plugins are written in.
- A standalone app is only useful to other apps if it ships a Windows virtual-camera driver,
  which is the hardest and most fragile part of every phone-webcam product.
- OBS already ships a virtual camera, so Discord or Meet still get the phone through OBS.
- The zoom controls also end up where the stream is composed.

**What flagship products do:**
- DroidCam ships both: a desktop client with its own webcam driver, and a separate OBS plugin.
- Camo and Elgato Camera Hub are standalone apps that bring their own virtual cameras.
- OBS has had a built-in Virtual Camera on Windows since 26.1.

**What it costs:**
- C++/Qt on the PC instead of Kotlin.
- OBS must be running.
- The plugin needs a rebuild for OBS major versions (the owner runs 31.1.2).
- The GPL applies (D-08).

**Further reading:**
- OBS plugin template (`obsproject/obs-plugintemplate`).
- libobs "Source API Reference" (docs.obsproject.com/reference-sources).
- obs-frontend-api `obs_frontend_add_dock_by_id`.
- For the road not taken: Windows 11's `MFCreateVirtualCamera` (Media Foundation virtual camera).

## D-02 The phone is the single owner of the zoom value (2026-11-26)

**The call:** the plugin sends zoom *requests*; only the phone applies zoom; both screens draw the
value the phone reports.

**Why:**
- Two writers separated by a network delay disagree. If both apply their own pinch and send it,
  each echo overwrites the other, and pinching on both at once becomes a tug of war.
- The phone is the only side that knows the camera's real range and actually moves the camera,
  so it is the natural authority.

**What flagship products do:**
- Multiplayer games use an authoritative server, as in Valve's Source engine: clients send
  inputs and the server's state wins.
- PTZ camera control (Sony VISCA) works the same way: the controller sends `Zoom Direct`, and the
  camera reports its real position on inquiry.
- Google Docs orders every edit through its server.

**What it costs:**
- Every PC-side change is a round trip, which is why D-03 exists.
- The PC can't change the zoom while disconnected.

**Further reading:**
- Gabriel Gambetta, "Fast-Paced Multiplayer" (client-server architecture, client-side prediction,
  server reconciliation).
- Yahn Bernier, "Latency Compensating Methods in Client/Server In-game Protocol Design and
  Optimization" (Valve, 2001).
- Sony VISCA command list (`CAM_Zoom`, `CAM_ZoomPosInq`).

## D-03 The dock preview predicts zoom; program output never does (2026-11-26)

**The call:** while you pinch or scroll in the dock, its preview scales the latest frame at once.
It hands over to the real zoomed video when that arrives. The OBS program output only ever shows
real frames.

**Why:**
- A round trip of 150-300 ms (dock, then phone, then encoder, then back) makes direct
  manipulation feel broken. About 0.1 s is the limit for "instant".
- Zoom here is a centre crop on both sides, so scaling the last frame is almost exactly what the
  phone will do, and it is nearly free.
- Keeping it out of program output means viewers never see an upscaled frame.

**What flagship products do:**
- Map apps (Google Maps, MapLibre) stretch the current tiles during a pinch and swap in sharp
  tiles when they load.
- Games predict locally and reconcile with the server.
- Touch UIs move content under the finger before new content is ready.

**What it costs:**
- The dock preview is briefly soft at high zoom.
- The prediction must clamp to the phone's reported range, or it will overshoot and snap back.

**Further reading:**
- Jakob Nielsen, "Response Times: The 3 Important Limits".
- Gambetta (D-02).
- Web Mercator tile pyramids, and how MapLibre GL JS renders overzoomed tiles.

## D-04 A capability report describes each phone; cameras are found with Camera2, including unlisted IDs (2026-11-26)

**The call:**
- On connect the phone reports its cameras, zoom ranges, lens stops, torch and quality modes,
  and nothing about a phone is hardcoded.
- Discovery uses Camera2 directly. It also tries camera IDs that `getCameraIdList()` doesn't
  return.
- Cameras are hidden by rule, never by ID: the physical members of an offered logical camera,
  and sensors whose largest output is under 720p.

**Why:**
- The owner's Huawei nova 10 lists only cameras `[0, 1]`, yet its logical camera 4 opens from an
  ordinary app.
- A probe on 2026-11-26 asked camera 4 for zoom 0.65 and got 0.65, and the active physical camera
  switched to 2, the ultrawide.
- CameraX only enumerates listed IDs, so it could never reach that lens.
- The owner asked that nothing be hardcoded, so that other phones with other zoom levels work.

**What flagship products do:**
- Open Camera adds a multi-camera switch on phones with several back cameras.
- The Blackmagic Camera app for Android lets you pick individual lenses.
- Google's own guidance is that logical multi-cameras expose zoom below 1.0 through
  `CONTROL_ZOOM_RATIO` (API 30+).
- OEMs vary in how much they expose, which is exactly why discovery has to probe.

**What it costs:**
- We own Camera2 session code (threading, lifecycle, rotation), which is more than CameraX
  needs.
- Opening unlisted IDs is undocumented behaviour that a future firmware may block. Discovery
  must fail quietly and fall back to the listed cameras.

**Further reading:**
- Android Developers, "Multi-camera API" (developer.android.com/media/camera/camera2/multi-camera).
- AOSP "Multi-Camera Support" (source.android.com/docs/core/camera/multi-camera).
- `CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE` and
  `CaptureResult.LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID`.

## D-05 Transport: one TCP connection per phone, Annex B video, drop to the next keyframe under backpressure (2026-11-26)

**The call:**
- Each phone keeps one TCP connection carrying control messages and video.
- Video is H.264, or HEVC when both ends support it, as an Annex B byte stream.
- When the phone's send queue backs up, it drops frames until the next keyframe instead of
  queueing them.

**Why:**
- On a LAN, TCP's latency is fine, and it gives ordering and reliability for free, with no custom
  retransmission.
- Its known failure is head-of-line blocking on bad Wi-Fi, where the queue grows and delay grows
  with it. Watching the queue and skipping to a keyframe caps the delay instead.
- The plugin listens and the phone connects, because the phone scans the PC's QR.

**What flagship products do:**
- DroidCam streams over TCP, with the phone as the server.
- scrcpy sends raw H.264 over a TCP socket forwarded through adb.
- SRT (Haivision) is UDP with retransmission, built for lossy and internet links.
- WebRTC uses UDP with Google Congestion Control.
- NDI|HX is the broadcast-LAN standard.

**What it costs:**
- It is worse than SRT or WebRTC on lossy Wi-Fi: no forward error correction, and no bitrate
  adaptation unless we add it.
- The PC listening means a Windows Firewall prompt, and failure on networks Windows calls Public.
  Both sides must name that cause.
- Moving to SRT later means adding a second transport.

**Further reading:**
- ITU-T H.264, Annex B (byte stream format).
- scrcpy `doc/develop.md` (its video socket protocol).
- The SRT protocol draft (IETF `draft-sharabayko-srt`).
- RFC 9000 (QUIC), on head-of-line blocking.
- IETF `draft-ietf-rmcat-gcc`.

## D-06 Layouts A, B and C are one dock: a preview toggle plus OBS's native floating (2026-11-26)

**The call:**
- One Qt dock. A button shows or hides its preview, which gives layouts A and B.
- It snaps out and back in through OBS's dock behaviour (drag the title, double-click it, or use
  its float button), which gives layout C.

**Why:**
- The owner liked all three. They are the same widget in three states.
- Three separate UIs would mean three things to keep in sync.
- Qt docks already float, re-dock and remember where they were.

**What flagship products do:**
- OBS's own docks work this way.
- So do Visual Studio's tool windows and Photoshop's panels (dock, float, collapse).

**What it costs:**
- Snapping is OBS's behaviour, so we can't add custom snap targets.
- The preview toggle has to be saved per dock.

**Further reading:**
- Qt `QDockWidget` (`DockWidgetFeatures`, `setFloating`).
- obs-frontend-api `obs_frontend_add_dock_by_id`.
- Qt Advanced Docking System (`githubuser0xFFFF/Qt-Advanced-Docking-System`), for a richer docking
  model.

## D-07 One repository holds the phone app, the plugin and the protocol (2026-11-26)

**The call:** `android/`, `obs-plugin/` and `protocol/` live in one repo, and both halves run the
same golden vectors.

**Why:**
- The protocol is the contract between the halves.
- In one repo, a change to it lands on both sides in one commit, and the shared vectors catch
  drift before a device ever does.

**What flagship products do:**
- scrcpy keeps its Android server (`server/`) and its C client (`app/`) in one repo.
- KDE Connect is the counterexample: `kdeconnect-android` and `kdeconnect-kde` are separate
  repos, and the protocol is versioned between them.

**What it costs:**
- Two toolchains in one tree (Gradle and CMake).
- CI has to build both.
- A release tag covers both halves.

**Further reading:**
- Potvin and Levenberg, "Why Google Stores Billions of Lines of Code in a Single Repository" (CACM,
  2016).
- Consumer-driven contract testing (the Pact docs), which is the alternative when the halves live
  apart.

## D-08 Licences: plugin GPL-2.0-or-later, phone app GPL-3.0 (2026-11-26)

**The call:** the OBS plugin is GPL-2.0-or-later, and the phone app is GPL-3.0.

**Why:**
- The plugin links libobs, which is GPL-2.0-or-later, so the plugin must be GPL-compatible.
- The phone app reuses Filet's QR code, which is GPL-3.0 and by the same author.
- Two programs that only talk over a socket are separate works, so the two licences don't
  conflict.

**What flagship products do:**
- OBS plugins such as DistroAV (obs-ndi) are GPL-2.0.
- scrcpy is Apache-2.0, because it doesn't link OBS.

**What it costs:**
- Anyone can fork and redistribute either half.
- A closed "pro" edition is impossible, which matches the no-paywall rule.

**Further reading:**
- GNU GPL FAQ, the entries on plugins (`#GPLPlugins`) and on mere aggregation (`#MereAggregation`).
- obs-studio's `COPYING`.
- The SPDX licence list.

## D-09 While streaming, the phone shows a black screen instead of turning the screen off (2026-11-26)

**The call:** a black-screen mode where the screen stays on, shows black with dim drifting text,
and wakes on a tap.

**Why:**
- An hour of camera, encoder and screen heats a phone, and the owner's already runs warm at idle.
- A black frame on an OLED draws almost nothing.
- Turning the screen off hands the app to the OS's background policy. Huawei is among the most
  aggressive at killing background work, even with a foreground service.

**What flagship products do:**
- Navigation and video-call apps (Google Maps navigation, Meet) keep the screen on with
  `FLAG_KEEP_SCREEN_ON` instead of betting on surviving in the background.

**What it costs:**
- The screen stays powered.
- An LCD phone saves much less than an OLED one.
- Static text risks burn-in, which is why the text drifts.

**Further reading:**
- dontkillmyapp.com (the Huawei section).
- Android "Foreground service types" (`camera`, `microphone`).
- `WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON`.
- OLED pixel-shifting.

## D-10 Orientation, mirror and flip: the phone decides, the plugin draws (2026-11-27)

**The call:**
- The phone sends frames exactly as the sensor produces them, plus three values in its STATE:
  `rot` (0, 90, 180 or 270: how far clockwise to turn the frame to make it upright), `mirror` and
  `flip`.
- The plugin applies them when it draws, as one matrix on the GPU. The OBS source reports its
  size already turned, so a portrait phone gives a 9:16 source and the scene item refits its box.
- `rot` follows the phone's accelerometer, and a Lock button (on both sides) freezes it.
- The phone owns all three, like zoom (D-02), so a press on either side lands on both.

**Why:**
- Turning a 4K frame on the phone means an extra GPU pass, or re-creating the encoder at a new
  size on every turn, which would mean a key frame, a hitch and more heat. On the PC it's one
  matrix multiply on a texture that is being drawn anyway.
- The formula is Android's own JPEG_ORIENTATION one: (sensor orientation ± device orientation),
  negated for front cameras. That makes it correct on any phone, not just the owner's.
- The phone preview uses the same transform, so the phone shows exactly what OBS shows (a front
  camera is not selfie-mirrored unless Mirror is on).

**What flagship products do:**
- Video files carry a rotation flag instead of rotated pixels: MP4's `tkhd` matrix, written by
  Android's `MediaRecorder.setOrientationHint`.
- WebRTC sends the same thing as the `urn:3gpp:video-orientation` RTP header extension (CVO),
  and the receiver turns the frame.
- Camo and DroidCam rotate on the desktop side as well.

**What it costs:**
- Any other consumer of the raw stream has to apply `rot` too. There is only one now, but a
  recorder added later must honour it.
- A turn of the phone changes the source's size, so a user layout that relied on a fixed size
  moves. Sources the plugin adds are fitted to the canvas (bounds: scale to inner), so they
  refit instead of overflowing.

**Further reading:**
- Android `CaptureRequest.JPEG_ORIENTATION` reference (it has the formula).
- `MediaRecorder.setOrientationHint`.
- ISO/IEC 14496-12, the track header matrix.
- 3GPP TS 26.114, Coordination of Video Orientation (CVO).
- libobs `gs_matrix_rotaa4f` and the scene item transform code (obs-scene.c).

## D-11 Updates come from GitHub Releases; each half updates itself (2026-11-27)

**The call:**
- A release has two files, `RelyACam-Setup-<ver>.exe` and `RelyACam-<ver>.apk`, under one tag.
- The phone app checks the releases API at most twice a day. It downloads the APK and hands it
  to Android's installer (ported from Filet's updater).
- The plugin checks once per OBS start. It downloads the installer and runs it, and the
  installer closes OBS through the Restart Manager.
- The plugin talks HTTPS through WinHTTP, not Qt, because OBS ships Qt without a TLS backend.
- `scripts/release.ps1` makes the release, as a draft unless `-Publish` is given.

**Why:**
- The owner asked for an updater in each half "like Filet's", and for no account and no cloud.
  A public release page is neither: it's the same page a person would open.
- One tag for both halves keeps the protocol versions matched (D-07).

**What flagship products do:**
- OBS itself updates through its own update server, with a signed manifest.
- F-Droid apps update through the store.
- Many GitHub-hosted desktop tools (Notepad++, ShareX) poll GitHub Releases the same way.

**What it costs:**
- There is no signature check beyond HTTPS and Android's own APK signature check. Android
  refuses an update signed with a different key, which protects the phone side. On the PC, a
  compromised GitHub account could ship a bad installer. Signed installers (Authenticode) are
  the fix, and they cost money.
- The GitHub API allows 60 unauthenticated calls an hour per IP, which is plenty for a check
  twice a day.
- **The release keystore must never be lost.** Without it, no future APK can update an
  installed one. Keep the backup next to the key, and keep another copy off this PC.

**Further reading:**
- GitHub REST API, "Releases" (`GET /repos/{owner}/{repo}/releases`).
- Android `PackageInstaller`, and APK Signature Scheme v2/v3.
- Microsoft Restart Manager.
- The Update Framework (TUF) specification: what a properly signed updater looks like.

## D-12 Each half points at the other: the PC serves the APK, USB installs it (2026-11-27)

**The call:**
- **PC to phone, three ways:**
  - The dock's "No app on the phone?" shows a QR to `http://<pc>:<port>/`. The plugin answers
    plain HTTP on the same port as the protocol: a first byte of `G` means GET, which can't be a
    frame type. The page serves the APK bundled with the plugin.
  - "Install app over USB" uses the bundled adb.
  - The release page.
- **Phone to PC:** "Get PC plugin" shows the release link, with Share and Copy.
- **USB pairing** forwards the port with `adb reverse` and opens the pairing link in the app with
  host 127.0.0.1.

**Why:**
- The owner asked for "one click setup from both sides" for someone who downloaded only one half.
- Serving from the PC works offline and on a hotspot, where the phone may have no internet.
- adb is the one Windows tool that can install an app and open a tunnel without a driver install
  (Windows Update supplies the USB driver for most phones).

**What flagship products do:**
- DroidCam and Iriun link to their apps from the desktop client.
- scrcpy ships adb next to itself and uses `adb reverse` for its socket.
- Syncthing serves its own web UI from the program itself.

**What it costs:**
- The plugin carries adb (about 6 MB) and the APK.
- The LAN page is plain HTTP, so anyone on the same network could fetch the APK. That's harmless,
  because it is the public app.
- USB needs Developer options turned on. The dock explains how when it finds no phone.

**Further reading:**
- scrcpy `doc/develop.md` (adb reverse/forward).
- Android "Run apps on a hardware device" (USB debugging).
- RFC 9112, the HTTP/1.1 message syntax used by the tiny server.

## D-13 The PC installer: per-machine into ProgramData, with firewall rules (2026-11-27)

**The call:**
- An Inno Setup installer puts the plugin in `%ProgramData%\obs-studio\plugins\rely-a-cam`.
- It adds two inbound rules for `obs64.exe` on every network profile: TCP 41127-41146 and UDP
  41126.
- It removes both rules on uninstall.
- If the rules are missing (a hand install), the dock shows "Allow phones", which adds the same
  rules through an admin prompt.

**Why:**
- Windows Firewall on a network marked Public (every phone hotspot) silently drops inbound
  connections. That is the most common reason a phone can't connect, so the installer fixes it
  before it happens.
- ProgramData is where OBS 28+ looks for third-party plugins, and it survives OBS reinstalls.

**What flagship products do:**
- OBS plugins such as DistroAV and Move Transition ship Inno Setup or NSIS installers to the same
  folder.
- Game launchers and Plex add firewall rules at install time.

**What it costs:**
- The installer needs admin rights (one UAC prompt).
- The rule is tied to OBS's path. Moving OBS needs a reinstall, or a press of "Allow phones".

**Further reading:**
- OBS "Plugin installation locations" (the ProgramData plugin path, OBS 28+).
- `netsh advfirewall firewall` reference.
- Inno Setup documentation: `[Run]`, `CloseApplications`.
