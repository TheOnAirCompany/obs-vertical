# Vertical Audio (macOS): the vertical mix as a microphone

Goal: TikTok LIVE Studio (and any app that records a microphone) sees a microphone named
**Vertical Audio** that carries the **vertical mix** of OBS in real time, aligned with the
image of **Vertical Camera**. It ships with the camera, in the same package
("Vertical Camera & Audio", `obs-vertical-camera-<version>-macos-universal.pkg`).

## 1. The vertical mix

OBS has one audio output with six mixer tracks; each source has one bit per track
(Edit > Advanced Audio Properties). Vertical manages **one track**, the vertical track:

* **Which track.** The first track, counting down from 6, that OBS's own outputs do not use
  (Settings > Output of the current profile: the stream track, the recording tracks, the
  Twitch VOD track; simple or advanced mode). The choice is kept with the scene
  collection and made again when the profile changes or an OBS stream or recording
  starts. When every track is used, track 6. Settings > Audio of Vertical shows it.
* **The rule.** For every audio input, the bit of the vertical track is
  `on track 1 (the main mix) AND not excluded`, kept live: sources created, loaded with a
  collection, tracks changed by the user (the `audio_mixers` signal is corrected before
  OBS stores the value), collection switch. **No other track is ever changed.**
* **The main mixer keeps control.** Mute and volume are global in OBS: a source muted or
  turned down in the mixer is muted or turned down in the vertical mix too.
* **Exclusions.** Settings > Audio lists every audio input of the 16:9 scenes (and the
  global devices, Desktop Audio and Mic/Aux), one checkbox each, "sent to vertical". New
  sources are sent by default. Typical use: untick the music so that a TikTok live gets
  no copyright strike while the main stream keeps it. Exclusions are saved with the scene
  collection, by source uuid (they survive renames).
* **Who uses it.** Vertical's streams, recording and replay buffer encode the vertical
  track (destination audio track "Vertical mix", the default; an explicit OBS track can
  still be chosen), the dock meter shows it, and the Vertical Audio microphone plays it.

Code: `src/core/audio-mix.*` (track choice, the rule, exclusions; unit-tested),
`src/obs/audio-mix.*` (signals, the list for the settings, the meter tap).

## 2. Architecture

```
OBS process                                     coreaudiod (sandboxed driver host)
------------------------------------------      --------------------------------------------
raw audio callback on the vertical track        VerticalAudio.driver (AudioServerPlugIn)
  (OBS audio thread, 1024-frame chunks,
   timestamp = OBS time of the first frame)       "Vertical Audio Feed"  hidden, output only
        |                                            WriteMix at output sample time T
        v                                                     |  shared ring (16384 frames)
  TimedRing (lock-free, 1.4 s)                                v
        |                                          "Vertical Audio"  visible, input only
  CoreAudio IO proc on the feed device              ReadInput reads T - 128 frames
  (256-frame buffer): for an output at host            |
  time H, plays the frames OBS stamped                 v
  H - (schedule + audio delay) ------------------>  apps: TikTok LIVE Studio, ...
```

### Why two devices, and does "Vertical Audio" show as an output?

No. A plain loopback device has an output stream and an input stream, so it shows in the
speaker lists too: anyone could send the system sound into it by mistake (and hear
nothing). The driver therefore publishes **two devices on one clock**:

* **Vertical Audio**: input only, visible: the microphone apps list.
* **Vertical Audio Feed**: output only, `kAudioDevicePropertyIsHidden = 1`: absent from
  every device list, still reachable by UID (`kAudioHardwarePropertyTranslateUIDToDevice`),
  which is how the plugin finds it.

Both devices take their zero time stamps from one anchor on the host clock at 48 kHz, so
the same sample time is the same instant on both; the feed writes the shared ring at its
output sample time, the input reads it 128 frames (2.7 ms) behind its input sample time
(margin for the feed's IO thread) and clears what it read: nothing is heard twice, and
the microphone falls silent when OBS stops. Writes are dropped while no app listens.

### Why not shared memory or XPC

Third-party audio drivers run inside coreaudiod's sandbox. Shared memory or a Mach
service between OBS (user session) and the driver (system domain) needs extra sandbox
exceptions and its own synchronisation and lifetime handling. The HAL already moves audio
between processes, with real-time threads and time stamps; playing into a device is the
supported, robust path, and it costs one IO buffer of latency.

### When it runs

The plugin watches `kAudioDevicePropertyDeviceIsRunningSomewhere` of the microphone: the
feed starts when an app records from "Vertical Audio" and stops when the last one stops.
Nothing runs otherwise (no IO, no power assertion). The device list is watched too, so
installing the driver while OBS runs works without a restart.

## 3. Latency and alignment with Vertical Camera

Both paths are measured against OBS timestamps (host clock, ns), which the audio mix and
the canvas frames share (`src/core/sync.hpp`):

| Audio path (OBS timestamp to "Vertical Audio") | |
| --- | --- |
| OBS hands a mixed chunk to the plugin | `lag` after the chunk ends (measured: the largest seen) |
| the whole chunk must be there | 1024 frames = 21.3 ms |
| one IO buffer of the feed device | 256 frames = 5.3 ms |
| thread wake-up margin | 2 ms |
| driver loopback | 128 frames = 2.7 ms |

The plugin plays the sample stamped `t` at host time `t + schedule`, where
`schedule = lag + 21.3 + 5.3 + 2` ms is the shortest that never runs dry; an underrun
adds 1 ms. So the audio latency is **fixed and known**. Measured from the moment OBS
hands a chunk over, its samples reach the microphone 10 ms (first sample) to 31 ms (last
sample) later: within the 30-40 ms target. Measured from the OBS timestamp, add the OBS
buffering (`lag`, a few ms to a few tens of ms depending on the sources).

| Video path (OBS timestamp to Vertical Camera clients) | |
| --- | --- |
| the frame reaches the camera output | `video lag` (measured on every frame; typically 1-2 frames) |
| the extension polls the sink at 120 Hz | 4.2 ms on average |

**Offset** (Settings > Audio, "Audio/video offset (ms)"): positive delays the audio,
negative delays the image, for the Vertical Camera + Vertical Audio pair only (Vertical's
streams and recordings are synchronised by OBS's muxers already). Its **default** is
`video path - audio path`, from the measured lags (typical values until measured): by
default both arrive together. "Default" puts it back; while the default is in use it
follows the measurements in steps of 5 ms. The value is per machine (OBS user config,
`[Vertical] SyncOffsetMs`, `SyncOffsetAuto`), not per collection.

How an offset is applied, near-zero CPU:

* **audio later**: the IO proc reads further back in the ring (no copy, no buffer);
* **image later**: the virtual camera output holds whole frames before handing them to
  the camera sink. They are the pooled IOSurface-backed pixel buffers themselves (no
  copy); memory only, at most 500 ms. The remainder below one frame goes to the audio,
  so the net shift is exact to the sample (`core::planSync`).

### Sync test

Settings > Audio > **Sync test**: a white flash on the vertical canvas and a 1 kHz beep
(100 ms, -6 dBFS) on the vertical track, stamped with the same OBS time, 300 ms after the
click, on every vertical output. Check it by eye and ear in TikTok LIVE Studio. Nothing
is added to the scenes: the flash is a private source on channel 1 of the vertical
canvas, the beep a private source on a free channel of the main view, on the vertical
track only (not in the main mix). The OBS time of the test is written to
`$TMPDIR/vertical-sync-test.json` for the measurement tool.

## 4. The driver

`camera-extension/audio-driver/VerticalAudio.cpp`, our own minimal AudioServerPlugIn
(C++17, no third-party code): plug-in object, two devices, one stream each, 48 kHz stereo
float32, no controls, no box. Bundle `VerticalAudio.driver`, id
`company.theonair.vertical-audio`, factory `VerticalAudio_Create`
(`7FA6C826-F230-4CFF-9B15-92D130BB9B32`). UIDs in `src/core/audio-device-ids.hpp`:

| Device | UID |
| --- | --- |
| Vertical Audio | `company.theonair.vertical-audio.input` |
| Vertical Audio Feed | `company.theonair.vertical-audio.feed` |

Never change them after a release: apps remember audio devices by UID.

Signing: Developer ID Application, hardened runtime, secure timestamp; no entitlement and
no provisioning profile. Built by `camera-extension/CMakeLists.txt` (target
`vertical-audio-driver`, universal), signed again and notarized with the app by
`scripts/sign-and-notarize.sh`, packaged by `scripts/make-pkg.sh`.

## 5. Install, uninstall

The package installs `Vertical Camera.app` into `/Applications` and `VerticalAudio.driver`
into `/Library/Audio/Plug-Ins/HAL` (both not relocatable), asks for an administrator
password, then restarts coreaudiod (`launchctl kickstart -k
system/com.apple.audio.coreaudiod`, else `killall coreaudiod`): the Mac's sound stops
for about a second. Check: `system_profiler SPAudioDataType` lists "Vertical Audio";
Audio MIDI Setup shows it as an input device.

Uninstall: `sudo rm -rf /Library/Audio/Plug-Ins/HAL/VerticalAudio.driver && sudo launchctl
kickstart -k system/com.apple.audio.coreaudiod`.

## 6. Testing

* Core unit tests: track choice, the rule, exclusions, meter math, schedule, default
  offset, offset plan, timed ring, loopback ring (`tests/test-audio-mix.cpp`,
  `test-meter.cpp`, `test-sync.cpp`).
* Driver host test (macOS, no install): `vertical-audio-driver-tests <module or .driver>`
  loads the driver like coreaudiod (CFPlugIn from its Info.plist, or dlopen) and drives
  its `AudioServerPlugInDriverInterface`: objects, UIDs, hidden feed, no output stream on
  the microphone, formats, shared clock (one period = 341.33 ms of host time), bit-exact
  loopback 128 frames later, read-once, no stale audio for a new listener.
  `ctest --test-dir build_tests` runs it on the module build.
* In OBS (`-DVERTICAL_SELFTEST=ON`): groups `audio` (track management, bits, mute and
  exclusion heard in the vertical mix, dock meter, vertical encoders on the vertical
  track, sync test, offset plan, persistence across a collection switch) and `vaudio`
  (feeds the installed driver; skipped when not installed).
* End to end, with the driver installed: **`vertical-sync-measure`** (built with the
  camera, `build_camera/Release/vertical-sync-measure`):

  ```sh
  # OBS: Vertical Settings open on the Audio tab (or OBS started with VERTICAL_SYNC_REMOTE=1),
  # Vertical Camera started from the dock if the camera is installed.
  build_camera/Release/vertical-sync-measure --count 10        # or --audio-only
  ```

  It opens "Vertical Camera" and "Vertical Audio" with AVFoundation like an app, posts the
  Darwin notification `company.theonair.vertical.sync-test` (the plugin runs a sync test),
  finds the flash in the frames and the beep in the audio, and prints per test and as a
  median: the audio/video offset (positive: sound after image), OBS → Vertical Audio and
  OBS → Vertical Camera. macOS asks once for microphone and camera access for the
  terminal that runs it.
