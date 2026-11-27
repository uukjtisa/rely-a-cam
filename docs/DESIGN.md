# Rely-A-Cam design standard

`docs/mock.html` (round 5, 2026-11-27) is the approved visual reference. This file pins the
values down, so a new screen or a new dock control matches without being re-derived by eye.

## New screen in ten lines

1. **Phone app:** dark, camera-first. Use only `Palette` (android `ui/Kit.kt`). Never a literal
   colour in a screen.
2. **Components** (all in `Kit.kt`):
   - Over the picture: `RoundIcon` (the live rail), `StatusLine` (the one status line),
     `ZoomValue`, `SlimRuler`, `GridOverlay`, `GestureMarks`, `LevelLine`, `LockBadge`, `FocusMark`, `EvSlider`,
     `StopButton`, `Pill` (a notice over the picture).
   - Sheets (Setup): `SectionTitle`, `SheetRow`, `SheetChip`, `ChipSet`, `SheetNote`.
   - Elsewhere: `RailButton` (the Mic only screen), `Chip`, `Primary`, `Secondary`, `Gap`,
     `Meter` (a mic level with a CLIP light), `Glyph` (the icon set).

   A new kind of component goes into `Kit.kt` (and this file) before any screen uses it.
3. **Camera image is full-bleed.** Controls float over it on `Palette.glass`. Nothing is
   letterboxed except the preview's own aspect. The status bar is hidden while live.
4. **One accent: blue.** Use `blue` for filled actions and `blueLight` for selected or on states.
   `red` is only for live, stop or a muted mic, `yellow` only for warnings (heat, firewall, a
   lock that's on), `green` only for connected (and `level` for a level phone). The one exception
   is a level meter, whose green, yellow and red zones mean loudness (OBS's own mixer uses the
   same), with a red CLIP light. The second is the gestures overlay (`GestureMarks`), drawn on the
   picture in Rely-A-Cam-Gestures' own colours so the phone, the dock and that app's window look
   alike: a `#5FD06C` skeleton with white joints, a ring that fills while a gesture is held on a
   `#88000000` track, the name on a `#B8000000` pill, and `yellow` only while a gesture waits for
   the confirm sign.
5. **Every state says what to do next.** An error names the cause and the fix (see the
   Unreachable screen). Never a bare "failed". Heat says what to try: "hot: try 1080p".
6. **Zoom values** are labelled with their real value (`0.65x`, `2x`), via `Presets.label` or
   `zoomLabel`. Never "wide" or "tele".
7. **Controls that can't work are hidden or disabled with a reason**, and an option whose
   parent is off isn't shown at all. No torch button on a camera without a flash; no Input or
   Clean-up while the mic is muted; no snapshot button on a camera with no still size. "This
   camera can't zoom" instead of a dead slider.
8. **OBS plugin:** colours from the palette OBS gives its widgets, so every OBS theme works.
   The fixed exceptions, each named in `dock.cpp`: the QR (black on white); OBS's mixer colours
   for the meter zones, CLIP and the muted-mic button; the warning yellow (`#E5AF24`, heat and
   locks in the status line, "can't zoom"); the note green (`#5FD06C`); the grid and the preview
   pills, which sit on the picture (white and black at fixed opacities, as on the phone).
9. **Icons, not words, on the tool buttons**, on both halves, always with a tooltip or content
   description. The icons are the mock's SVGs (`IC` in `docs/mock.html`): `Glyph` paths on the
   phone, QtSvg in the dock. No icon fonts. Actions inside menus and sheets stay words.
10. **Text** is plain and short, and says what happens ("Install app over USB", not "Deploy").

## Phone palette (`Palette` in `android/app/.../ui/Kit.kt`)

These are taken from OBS 31's Yami theme, so the two halves read as one product.

| Token | Value | Use |
|---|---|---|
| `bg` | `#13141A` | screen background, behind the camera |
| `panel` | `#1D1F26` | dialogs |
| `base` | `#272A33` | inputs, list rows |
| `line` | `#3C404D` | borders, dividers |
| `hover` | `#464B59` | pressed or hover state |
| `blue` | `#284CB8` | primary filled button |
| `blueLight` | `#476BD7` | selected and on states, focus ring, links |
| `red` | `#C01C37` | live dot, Stop, muted mic |
| `green` | `#25A231` | connected dot |
| `yellow` | `#E5AF24` | warnings: heat, firewall, locks |
| `text` | `#EDEDED` | body text |
| `muted` | `#999999` | secondary text |
| `scrim` | `#99000000` | older pills over the camera image |
| `glass` | `#AA000000` | round-5 controls over the picture (rail, status line, zoom value) |
| `glassEdge` | `#26FFFFFF` | their 1 dp border when off |
| `sheet` | `#F20B0C10` | the Setup sheet over the picture |
| `sheetLabel` | `#9AA0AA` | section titles in the sheet |
| `sheetMuted` | `#8A8F99` | small print in the sheet |
| `sheetRow` | `#10FFFFFF` | rows and chips in the sheet |
| `level` | `#4CD964` | the level line once it's level |
| `onImage` | `#FFFFFF` | text and marks drawn on the picture |
| `shadow` | `#CC000000` | their shadow |

## Shapes and sizes

| Thing | Value |
|---|---|
| Rail buttons (`RoundIcon`) | 48 dp circles, 24 dp icon, `glass` fill, `glassEdge` border off, `blueLight` on; 40 dp in the portrait bottom bar |
| Status line | `glass` pill, one line, fonts +1 over the mock (the mock phone is drawn at about 2/3 of real dp) |
| Slim ruler | 36 dp tall; labelled ticks at the presets and the maximum, small ticks at each whole step between, a white handle |
| Grid | rule of thirds, 1 px white at 55% with a dark edge, on the phone, the dock and the output alike |
| Gestures overlay | skeleton 2.5 dp, joints 2.2 dp, wrist 3.5 dp; ring 19 dp radius, 5 dp wide, 30 dp under the hand; pill 12 sp semibold, 26 dp under the ring. The same numbers in px in the dock, scaled with its preview (0.5x to 1x). The output framing shows in the dock as a dashed white box at the grid's opacity |
| Pills, chips | fully rounded (`RoundedCornerShape(50)`), 10 dp × 5 dp padding |
| Sheet rows | 8 dp corners, `sheetRow` fill, 1 dp `blueLight` outline when chosen |
| Meter | 6 dp bar (10 dp on the Mic only screen); zones at 65% and 85% of full scale; CLIP latched 1 s |
| Buttons | 6 dp corner radius |
| Focus ring (tap to focus) | `blueLight`, at the tap; at the centre for a lock from the PC |
| Dock tool buttons | 28 × 26 px, 15 px icon, 4 px radius, OBS's `Button` colour, `Highlight` when on |
| Dock ruler | 30 px tall, 12 px inset each side, 14 px handle, labels 10 px semibold |

## Motion

| Motion | Value | Where |
|---|---|---|
| Zoom glide for PC requests | log-space exponential, τ = 70 ms, ticked at about 60 Hz | `CameraController.GLIDE_TAU` |
| Phone pinch | no glide (it follows the fingers) | `Engine.pinch` |
| Dock prediction | shown until the phone reports the value, at most 2 × RTT + 350 ms; a dashed yellow ring on the zoom pill meanwhile | `PhoneDock::refresh` |
| Focus ring and brightness slider | shown 3 s after a tap; stay while locked | `FOCUS_UI_MS` in `Screens.kt` |
| Lock focus + exposure | a press held 600 ms; exposure locks 450 ms later, once metering has settled | `LOCK_HOLD_MS` in `CameraView.kt`, `CameraController.focusAt` |
| Black screen text | slow drift, so the panel doesn't burn in | `BlackScreen` |
| Mic meters | a new reading every 50 ms; the dock's falls back by 20% a tick and rises at once | `AudioEncoder.feed`, `PhoneDock::tickMeter` |
| Dock note line | shown 9 s | `PhoneDock::message` |

## Dock (OBS plugin)

The dock is one widget (`obs-plugin/src/dock/dock.cpp`), the mock's `dockControlsHTML`. From top
to bottom:

1. phone chips (a green light when connected, a ring when not) and **+ Add phone**;
2. one status line: link · quality · delay · temperature (yellow with "hot: try ..." from 45 °C)
   · "AE/AF locked" when it is; codec, bit rate, address and battery are in its tooltip;
3. the preview with the grid (when on for the dock), the zoom pill and the hint "scroll · pinch ·
   right-click: menu"; or the Add phone panel in its place;
4. the slim ruler, or "This camera can't zoom (1x only)";
5. the tools: mic (blue on, red muted) with the meter and CLIP, or "mic muted"; torch (only with
   a flash); snapshot (only when the camera has a still size); ⋯ for the menu;
6. Allow phones and Update, only when needed;
7. the note line.

**The menu** (⋯, or right-click on the preview) is the mock's `dockMenu`: Camera ▸, Quality ▸
(and the codec when there's a choice), Picture ▸ (Mirror, Flip, Lock turn; Lock focus +
exposure, White balance ▸; Snapshot to PC), Mic ▸ (Mic on; while on: Input ▸, Clean-up ▸, Every
scene, Use in other apps with "...and be Windows' default mic" and Remove VB-Cable..., or Use in
other apps... to install it; Mic only), Overlays ▸ Grid ▸ (Dock, Phone, OBS output), Gestures ▸ (On
for this phone, the detection overlay and the face map, each in the dock and on the phone or on
the OBS output, Open the gestures editor..., and the app's status), then Show
preview and Connect a phone over USB. Every scene and Use in other apps are about the OBS
source, so they stay while the phone is offline.

**Menus stay open** on a tick or a pick: the owner never has to reopen a submenu. An open menu
refills from live state, and a click reads the state at that moment, not the one the menu opened
with. Items that open a window or do a one-off still close it.

On the phone, **Setup** (the gear, or a tap on the status line) is a sheet over the picture on
`sheet`: Mic (inputs and clean-up only while on), Video (quality, the codec when there's a
choice, then codec, bit rate and battery as small print), Picture (Mirror, Flip, Lock turn, White
balance, Snapshot to PC), Overlays (Grid on Dock, Phone, OBS output; Level line, this phone
only). Mic only replaces the picture with the mic: its input, a big meter and "Turn the camera
back on"; the rail keeps only Mic, Setup and Dark.

Hiding the preview (Show preview in the menu) gives the slim layout. Floating the dock is OBS's
own dock behaviour, which gives the pop-out window.
