# Rely-A-Cam design standard

`docs/mock.html` (round 4, 2026-11-27) is the approved visual reference. This file pins the
values down, so a new screen or a new dock control matches without being re-derived by eye.

## New screen in ten lines

1. **Phone app:** dark, camera-first. Use only `Palette` (android `ui/Kit.kt`). Never a literal
   colour in a screen.
2. **Components:**
   - `Pill`: status over the camera image.
   - `RailButton`: round toggles on the right rail.
   - `Chip`: presets, picks.
   - `Primary` and `Secondary`: actions.
   - `Gap`: spacing.
   - `Meter`: a mic level with a CLIP light.
   - `SectionTitle` and `SheetRow`: headings and choices inside a sheet (Setup).

   A new kind of component goes into `Kit.kt` (and this file) before any screen uses it.
3. **Camera image is full-bleed.** Controls float over it on `Palette.scrim`. Nothing is
   letterboxed except the preview's own aspect.
4. **One accent: blue.** Use `blue` for filled actions and `blueLight` for selected or on states.
   `red` is only for live or stop, `yellow` only for warnings (heat, firewall), `green` only for
   connected. The one exception is a level meter, whose green, yellow and red zones mean loudness
   (OBS's own mixer uses the same), with a red CLIP light.
5. **Every state says what to do next.** An error names the cause and the fix (see the
   Unreachable screen). Never a bare "failed".
6. **Zoom values** are labelled with their real value (`0.65x`, `2x`), via `Presets.label` or
   `zoomLabel`. Never "wide" or "tele".
7. **Controls that can't work are hidden or disabled with a reason.** No torch button on a
   camera without a flash. "This camera can't zoom" instead of a dead slider.
8. **OBS plugin:** no hardcoded colours. Qt widgets use the palette OBS gives them, so every
   OBS theme works. The exceptions are the QR, which is always black on white, and the mic
   meter's zones and CLIP light (`MeterWidget`).
9. **Dock density:** OBS's own control sizes, with 6 px margins and spacing. Text labels on
   buttons, no icon fonts.
10. **Text** is plain and short, and says what happens ("Install app over USB", not "Deploy").

## Phone palette (`Palette` in `android/app/.../ui/Kit.kt`)

These are taken from OBS 31's Yami theme, so the two halves read as one product.

| Token | Value | Use |
|---|---|---|
| `bg` | `#13141A` | screen background, behind the camera |
| `panel` | `#1D1F26` | dialogs, sheets |
| `base` | `#272A33` | inputs, list rows |
| `line` | `#3C404D` | borders, dividers |
| `hover` | `#464B59` | pressed or hover state |
| `blue` | `#284CB8` | primary filled button |
| `blueLight` | `#476BD7` | selected and on states, focus ring, links |
| `red` | `#C01C37` | live dot, Stop |
| `green` | `#25A231` | connected dot |
| `yellow` | `#E5AF24` | warnings: heat, firewall |
| `text` | `#EDEDED` | body text |
| `muted` | `#999999` | secondary text |
| `scrim` | `#99000000` | pills and rail over the camera image |

## Shapes and sizes

| Thing | Value |
|---|---|
| Pills, chips | fully rounded (`RoundedCornerShape(50)`), 10 dp × 5 dp padding |
| Rail buttons | circles; border 1 dp, `#33FFFFFF` when off, `blueLight` when on |
| Sheet rows | 8 dp corners, `base` fill, 1 dp `blueLight` outline when chosen |
| Meter | 6 dp bar (10 dp on the Mic only screen); zones at 65% and 85% of full scale; CLIP latched 1 s |
| Buttons | 6 dp corner radius |
| Focus ring (tap to focus) | `blueLight`, fades out over about 0.8 s |

## Motion

| Motion | Value | Where |
|---|---|---|
| Zoom glide for PC requests | log-space exponential, τ = 70 ms, ticked at about 60 Hz | `CameraController.GLIDE_TAU` |
| Phone pinch | no glide (it follows the fingers) | `Engine.pinch` |
| Dock prediction | shown until the phone reports the value, at most 2 × RTT + 350 ms | `PhoneDock::refresh` |
| Black screen text | slow drift, so the panel doesn't burn in | `BlackScreen` |
| Mic meters | a new reading every 50 ms; the dock's falls back by 20% a tick and rises at once | `AudioEncoder.feed`, `PhoneDock::tickMeter` |

## Dock (OBS plugin)

The dock is one widget (`obs-plugin/src/dock/dock.cpp`). From top to bottom:

1. phone chips (● connected, ○ offline) and **+ Add phone**;
2. the status line;
3. the preview, or the Add phone panel in its place;
4. presets, then the log slider labelled with the phone's own min and max;
5. a row with camera and Torch;
6. a row with quality, codec, Mirror, Flip and Lock;
7. the Mic block, a labelled grid: **Mic** (On/Muted and the meter), **Input**, **Clean-up**
   (only the effects the phone has), **Route** (Every scene, Mic only, Use in other apps). It
   sits outside the camera controls, so Every scene and Use in other apps work while the phone
   is offline; Mic only disables the camera controls;
8. a footer with Preview, USB, Allow phones (only when needed) and Update (only when available).

On the phone, **Setup** (a rail button, or a tap on the quality line) is a full-screen sheet on
`panel`: a Mic section (on with its meter, inputs, clean-up chips, Mic only) and a Video section
(quality and codec chips). Mic only replaces the picture with the mic: its input, a big meter
and "Turn the camera back on"; the rail keeps only Mic, Setup and Dark.

Hiding the preview (the Preview toggle) gives the slim layout B. Floating the dock is OBS's own
dock behaviour, which gives layout C.
