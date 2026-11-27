# Rely-A-Cam wire protocol, version 1

One TCP connection per phone. The PC (the OBS plugin) listens and the phone connects. Every
multi-byte integer is big-endian. Text is UTF-8. JSON payloads are single objects; unknown keys
are ignored, so either side can add fields without a version bump.

Both halves implement this file once: `android/core/protocol` and `obs-plugin/src/net`. The
cases in `vectors/` are the contract; a change here changes both sides and the vectors in the
same commit.

## Ports

| Port | Use |
|---|---|
| TCP 41127 | Default listening port. If it is taken, the plugin picks a free one and keeps it. The QR always carries the real port. |
| UDP 41126 | Discovery (typed code, reconnect). |

The installer adds Windows Firewall rules for `obs64.exe` on exactly these two ports, on every
network profile. That is what makes a phone hotspot (a "Public" network to Windows) work.

## Pairing payload (the QR)

```
relyacam://pair?v=1&h=192.168.1.5,10.0.0.2&p=41127&k=<secret>&id=<pcId>&n=<pcName>
```

| Key | Meaning |
|---|---|
| `v` | protocol version |
| `h` | every IPv4 address of the PC's active interfaces, comma-separated, no loopback |
| `p` | TCP port |
| `k` | the PC's secret, 22 characters of base64url (128 bits), kept across restarts |
| `id` | the PC's id, random, kept across restarts |
| `n` | the PC's name, percent-encoded |

The phone tries every host in `h`, and the first that answers wins. A phone that has the secret
may connect; the dock shows the QR, and seeing the QR is the authorisation. Over USB the plugin
hands the phone the same link with `h=127.0.0.1`, after `adb reverse`.

The app opens `relyacam://` links itself, so any QR scanner that opens links also works.

## Discovery (UDP 41126)

A phone sends one datagram to the broadcast address of each of its networks:

| Request | Reply (unicast, JSON) |
|---|---|
| `RAC1?C` + 6-character code | the full pairing payload as JSON (`v,h,p,k,id,n`). Answered only while the dock's Add-phone panel is open, because the code is a short-lived PIN. |
| `RAC1?I` + pcId | `{"v":1,"h":[...],"p":41127,"id":"..."}`, without the secret. Used to find a PC that has already paired after its address changed. |

The code alphabet is `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no 0/O, 1/I); it is shown as `XXX-XXX`
and the dash is ignored.

## Frames

```
frame   := type:u8  length:u32  payload[length]          length <= 16 MiB
media   := pts:u64 (microseconds)  flags:u8  data         (VIDEO and AUDIO payloads)
flags   := 1 = key frame, 2 = codec config
```

| Type | Name | Direction | Payload |
|---|---|---|---|
| 0x01 | HELLO | phone to PC, first frame | JSON `{"v":1,"k":secret,"device":id,"name":"nova 10","app":"0.1.0"}` |
| 0x02 | WELCOME | PC to phone | JSON `{"v":1,"pc":name,"pcId":id,"source":"Phone - nova 10","codecs":["h264","hevc","aac"],"restore":{...}}` |
| 0x03 | CAPS | phone to PC | JSON capability report (below), sent after WELCOME and whenever it changes |
| 0x04 | STATE | phone to PC | JSON full state (below), on every change, at most about 30 a second |
| 0x05 | CMD | PC to phone | JSON with any subset of the state keys to change, or `{"keyframe":true}` |
| 0x06 | PING | either | 8 bytes, the sender's clock in microseconds |
| 0x07 | PONG | either | the PING payload echoed |
| 0x08 | BYE | either | empty; the sender is closing on purpose |
| 0x0F | ERROR | either | JSON `{"code":"auth"|"version"|"busy","msg":"..."}`, then close |
| 0x10 | VCONF | phone to PC | JSON `{"codec":"h264"|"hevc","w":1920,"h":1080,"fps":30}`, before the first VIDEO of a new stream |
| 0x11 | VIDEO | phone to PC | media: Annex B access unit (start codes included). A config frame carries SPS/PPS (and VPS) |
| 0x12 | ACONF | phone to PC | JSON `{"codec":"aac","rate":48000,"ch":1}` |
| 0x13 | AUDIO | phone to PC | media: one raw AAC access unit. A config frame carries the AudioSpecificConfig |

`restore` in WELCOME has the same keys as CMD. It holds what OBS remembered for this phone's
source (camera, zoom per camera, mode, mirror, flip, lock). The phone applies it, because OBS
wins on reconnect.

## Capability report (CAPS)

```json
{"device":"nova 10",
 "cams":[{"id":"4","facing":"back","label":"Back","min":0.65,"max":10.0,"lenses":[0.65,1.0],
          "flash":true,"modes":["1280x720@30","1920x1080@30","3840x2160@30"]}],
 "codecs":["h264","hevc"],"mic":true,
 "mics":[{"id":"15:bottom","label":"Bottom mic"},{"id":"15:back","label":"Back mic"}],
 "clean":["ns","aec","agc"]}
```

- `mics` lists the audio inputs the phone has right now: its built-in mics, plus a wired, USB or
  Bluetooth input while one is connected. The phone sends a new CAPS when that list changes.
  An `id` is only meaningful to the phone that sent it.
- `clean` lists the voice clean-up effects the phone has: `ns` (noise suppression), `aec` (echo
  cancellation), `agc` (automatic gain control). An effect not listed can't be turned on.

Nothing about a camera exists on the PC unless it is in this report. Presets are derived from it
the same way on both sides:

> the lens stops inside `[min, max]`, plus 2x when 2x is inside the range and not within 20% of a
> lens stop; a camera with `min == max` has no zoom controls.

## State (STATE)

```json
{"cam":"4","zoom":2.4,"torch":false,"mic":true,"mode":"1920x1080@30","codec":"h264",
 "rot":90,"mirror":false,"flip":false,"lock":false,"temp":41.2,"battery":78,"live":true,
 "micIn":"15:bottom","ns":true,"aec":false,"agc":false,"micOnly":false}
```

- `micIn` is the input in use, from `mics`. `ns`, `aec` and `agc` say which clean-up effects are
  on.
- `micOnly` means the camera is off and only the mic streams: no VIDEO frames arrive, and the PC
  shows nothing for that camera until it's false again.

- `zoom` is the zoom the camera is at now, which is not necessarily the last one requested. The
  phone owns it.
- `rot` is how far, clockwise, the PC must turn the decoded frame to show it upright (0, 90, 180
  or 270). The phone works it out from the sensor's mounting, the device's orientation and the
  lens facing. While `lock` is true it stops following the device.
- `mirror` (left-right) and `flip` (upside-down) are applied after `rot`, in the upright picture.
- Frames are sent as the sensor produces them, and the PC does the turning when it draws. That
  costs the phone nothing.

## Sequence

```
phone                                   PC
  HELLO  ------------------------------>
         <------------------------------  WELCOME (restore)   or ERROR auth, close
  CAPS   ------------------------------>
  STATE  ------------------------------>
  VCONF, VIDEO(config), VIDEO(key) ...->
         <------------------------------  CMD {"zoom":2.0}
  STATE {"zoom":1.1 ... 2.0} (glides) ->
  PING / PONG either way, every 2 s; 6 s without any frame = dead link
```

## Backpressure

The phone never lets video queue. When more than about 250 ms of video is waiting to be sent, it
drops frames up to the next key frame and asks its encoder for one. A late frame is worse than a
missing one.
