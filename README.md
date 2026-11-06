<div align="center">

# Vertical for OBS

[![Core tests](https://github.com/TheOnAirCompany/obs-vertical/actions/workflows/core-tests.yaml/badge.svg?branch=main)](https://github.com/TheOnAirCompany/obs-vertical/actions/workflows/core-tests.yaml) [![OBS Studio](https://img.shields.io/badge/OBS%20Studio-33.0.1%2B-302E31?logo=obsstudio&logoColor=white)](https://obsproject.com) [![Languages](https://img.shields.io/badge/languages-English%20%7C%20Fran%C3%A7ais-blue)](data/locale) [![License](https://img.shields.io/badge/license-GPL--2.0-blue)](LICENSE)

**Your show, in 9:16 too. Without doubling the CPU.**

Vertical for OBS is a native OBS Studio plugin that adds a **vertical canvas** with its own scenes next to your 16:9 one, and outputs it: **TikTok, YouTube, Instagram, Facebook and custom RTMP/SRT at the same time**, a **recording**, a **replay buffer** and a **9:16 virtual camera** for TikTok LIVE Studio — with hardware encoders shared between destinations.

</div>

> **Status: 0.1.0, first version.** For OBS Studio 33.0.1. Tested on macOS inside OBS: canvas, scenes, recording and real streaming to three RTMP servers sharing one hardware encoder. Windows and Linux builds pass CI but have not run inside OBS yet. The 9:16 virtual camera ships as a separate signed package; its activation has not been tested on a real Mac yet ([docs/VIRTUAL-CAMERA.md](docs/VIRTUAL-CAMERA.md)). Keep a backup of your scene collection.

## What it adds

### A vertical canvas, saved with your scene collection

A second OBS 33 canvas, 1080 × 1920 by default (720 × 1280, 1440 × 2560, 2160 × 3840, 4:5, 1:1 or any even size), with an optional smaller output size. It has its own scenes; your cameras and media are the **same sources** as in your 16:9 scenes, so they are captured and decoded once. The canvas and its scenes are saved with the scene collection like your other scenes.

### The Vertical dock

**Docks > Vertical**: a second production space for the vertical output.

- **Studio** layout when the dock is wide (two columns) and **Compact** when it is narrow (one column), chosen by the dock width; the ⋯ menu can force one.
- Header with the canvas size and frame rate, the **Linked scenes** switch and the ⋯ menu.
- Live preview with a **LIVE** pill and the aspect tag, a red frame while a destination is live; collapse it, or double-click it to open it in its own window.
- Vertical scenes: click a row to put it on air (the program scene shows in red with a **PROGRAM** badge), drag to reorder, and the link icon on each row to link it to 16:9 scenes.
- Destinations with their platform (the tile follows the server: a TikTok server shows TikTok), live status (connecting, live with time and bitrate, reconnecting with the attempt, error, stopped), a settings button, a ▶ "Go live" / ■ "Stop" button and ⋯ (edit, duplicate, delete); **Start all** / **Stop all**.
- Recording, replay buffer and virtual camera: tabs in Studio with a large **Stop Recording** button and timer, three compact lines in Compact.
- Status bar: outputs live, CPU, version and settings, and a warning when the video is encoded by the CPU (no hardware encoder).

### Linked scenes

Link 16:9 scenes to a vertical scene with the link icon of its row in the dock (a menu with checkmarks; several 16:9 scenes can share one vertical scene), or from another plugin. With **Linked scenes** on, switching the main program also switches the vertical output. Scenes without a link leave the vertical output where it is.

### Several destinations at once

Add TikTok, YouTube, Instagram, Facebook, a custom RTMP/RTMPS server or SRT. Each destination starts and stops on its own and shows its state (connecting, live with time and bitrate, reconnecting, error). Servers and keys are checked before starting (Instagram and Facebook need RTMPS, SRT needs a port, bitrate limits per platform). Destinations with the same encoder settings share **one video encoder and one audio encoder**.

### Recording and replay buffer

Record the vertical program in hybrid MP4 (safe if OBS stops unexpectedly), hybrid MOV, MKV, MP4 or MOV. The replay buffer keeps the last seconds and saves them on a click or a hotkey. When the recording uses the same bitrate as a destination, it reuses its encoder.

### Light on the CPU

Hardware encoders first (Apple VideoToolbox on Mac), x264 only when there is no hardware encoder. The vertical scene is rendered **only while something needs it**: an output, the visible preview or the virtual camera. See [Architecture](docs/ARCHITECTURE.md#3-cpu-strategy).

### 9:16 virtual camera and microphone (macOS)

A separate camera, **Vertical Camera**, for TikTok LIVE Studio and other apps, independent from OBS's own virtual camera (which stays on the 16:9 program). It uses a CoreMediaIO camera extension installed by a small companion app. See [Virtual camera](docs/VIRTUAL-CAMERA.md).

With it, a microphone, **Vertical Audio**, carries the vertical mix in real time, aligned with the camera image (an audio/video offset setting and a sync test help check it). See [Vertical Audio](docs/VIRTUAL-AUDIO.md).

### Vertical audio mix

Vertical manages one OBS mixer track for the vertical output: the sources of your main mix, minus the ones you untick in **Settings > Audio** (for example the music, to avoid a copyright strike on TikTok). Mute and volume in the OBS mixer apply to it too. Vertical's streams and recordings use it, and a thin stereo meter under the dock preview shows its level.

### Hotkeys

In **Settings > Hotkeys**: start/stop all streams, start/stop each destination, start/stop recording, start/stop and save the replay buffer, and switch to each vertical scene.

## Install

Download the file for your system from the [releases](https://github.com/TheOnAirCompany/obs-vertical/releases). OBS Studio **33.0.1 or later** is required.

| System | File | How to install |
|---|---|---|
| macOS 13+ (Apple Silicon and Intel) | `obs-vertical-<version>-macos-universal.pkg` | Open the package and follow the installer (it installs for the current user). Signed and notarized by Apple. |
| macOS, camera and microphone (optional) | `obs-vertical-camera-<version>-macos-universal.pkg` | **Vertical Camera & Audio**: installs **Vertical Camera** into Applications and the **Vertical Audio** microphone driver into /Library/Audio/Plug-Ins/HAL (administrator password; the Mac's sound stops for about a second while the audio system restarts). For the camera, open Vertical Camera once and click Install, then allow it in System Settings > General > Login Items & Extensions > Camera Extensions (macOS asks once). |
| Windows 10/11 x64 | `obs-vertical-<version>-windows-x64.zip` | Extract the `obs-vertical` folder into `C:\ProgramData\obs-studio\plugins\`. |
| Ubuntu x86_64 | `obs-vertical-<version>-x86_64-linux-gnu.deb` | `sudo apt install ./obs-vertical-<version>-x86_64-linux-gnu.deb`. Needs the `obs-studio` package **33 or later** installed system-wide. |

Restart OBS and open **Docks > Vertical**.

To uninstall, remove the plugin. The vertical canvas and its scenes stay in your scene collection (OBS keeps them, hidden from the main scene list) and come back when the plugin is installed again.

## Documentation

| Document | Content |
|---|---|
| [Architecture](docs/ARCHITECTURE.md) | Layers, canvas, CPU strategy, outputs, persistence, threads |
| [Integration](docs/INTEGRATION.md) | Proc handlers for other plugins (`vertical_get_canvas`, `vertical_link_scene`) |
| [Virtual camera](docs/VIRTUAL-CAMERA.md) | Camera extension design, signing, notarization, Apple Developer steps |
| [Vertical Audio](docs/VIRTUAL-AUDIO.md) | Vertical mix, audio driver, latency and audio/video offset, sync test and measurement |

## Building

The build system comes from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate), set up for OBS Studio 33.0.1 like [Layouts for OBS](https://github.com/TheOnAirCompany/obs-layouts).

```sh
cmake --preset macos          # or windows-x64, ubuntu-x86_64
cmake --build build_macos --config RelWithDebInfo
```

Options: `-DVERTICAL_VIRTUALCAM=OFF` leaves out the virtual camera output; `-DVERTICAL_BUILD_CAMERA=ON` also builds the camera extension and its host app (Xcode generator, see the virtual camera document for signing).

Core unit tests (no OBS needed):

```sh
cmake -S tests -B build_tests && cmake --build build_tests && ./build_tests/vertical-core-tests
```

## License

[GPL-2.0-or-later](LICENSE), like OBS Studio.

© 2026 The OnAir Company
