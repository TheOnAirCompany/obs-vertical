<div align="center">

# Vertical for OBS

[![Core tests](https://github.com/TheOnAirCompany/obs-vertical/actions/workflows/core-tests.yaml/badge.svg?branch=main)](https://github.com/TheOnAirCompany/obs-vertical/actions/workflows/core-tests.yaml) [![OBS Studio](https://img.shields.io/badge/OBS%20Studio-33.0.1%2B-302E31?logo=obsstudio&logoColor=white)](https://obsproject.com) [![Languages](https://img.shields.io/badge/languages-English%20%7C%20Fran%C3%A7ais-blue)](data/locale) [![License](https://img.shields.io/badge/license-GPL--2.0-blue)](LICENSE)

**Your show, in 9:16 too. Without doubling the CPU.**

Vertical for OBS is a native OBS Studio plugin that adds a **vertical canvas** with its own scenes next to your 16:9 one, and outputs it: **TikTok, YouTube, Instagram, Facebook and custom RTMP/SRT at the same time**, a **recording**, a **replay buffer** and a **9:16 virtual camera** for TikTok LIVE Studio — with hardware encoders shared between destinations.

</div>

> **Status: 0.1.0, first development version.** Builds against OBS Studio 33.0.1 on macOS (universal). Not yet tested inside a running OBS. The virtual camera needs a signed camera extension that is not distributed yet ([docs/VIRTUAL-CAMERA.md](docs/VIRTUAL-CAMERA.md)). Keep a backup of your scene collection.

## What it adds

### A vertical canvas, saved with your scene collection

A second OBS 33 canvas, 1080 × 1920 by default (720 × 1280, 1440 × 2560, 2160 × 3840, 4:5, 1:1 or any even size), with an optional smaller output size. It has its own scenes; your cameras and media are the **same sources** as in your 16:9 scenes, so they are captured and decoded once. The canvas and its scenes are saved with the scene collection like your other scenes.

### The Vertical dock

**Docks > Vertical**: a live preview of the vertical program, the list of vertical scenes (add, remove, rename, reorder by dragging, click to put on air) and every output with its status.

### Linked scenes

Link a 16:9 scene to a vertical scene (↔ in the dock, or from another plugin). With **Linked scenes** on, switching the main program also switches the vertical output. Scenes without a link leave the vertical output where it is.

### Several destinations at once

Add TikTok, YouTube, Instagram, Facebook, a custom RTMP/RTMPS server or SRT. Each destination starts and stops on its own and shows its state (connecting, live with time and bitrate, reconnecting, error). Servers and keys are checked before starting (Instagram and Facebook need RTMPS, SRT needs a port, bitrate limits per platform). Destinations with the same encoder settings share **one video encoder and one audio encoder**.

### Recording and replay buffer

Record the vertical program in hybrid MP4 (safe if OBS stops unexpectedly), hybrid MOV, MKV, MP4 or MOV. The replay buffer keeps the last seconds and saves them on a click or a hotkey. When the recording uses the same bitrate as a destination, it reuses its encoder.

### Light on the CPU

Hardware encoders first (Apple VideoToolbox on Mac), x264 only when there is no hardware encoder. The vertical scene is rendered **only while something needs it**: an output, the visible preview or the virtual camera. See [Architecture](docs/ARCHITECTURE.md#3-cpu-strategy).

### 9:16 virtual camera (macOS)

A separate camera, **Vertical Camera**, for TikTok LIVE Studio and other apps, independent from OBS's own virtual camera (which stays on the 16:9 program). It uses a CoreMediaIO camera extension installed by a small companion app. See [Virtual camera](docs/VIRTUAL-CAMERA.md).

### Hotkeys

In **Settings > Hotkeys**: start/stop all streams, start/stop each destination, start/stop recording, start/stop and save the replay buffer, and switch to each vertical scene.

## Install

OBS Studio **33.0.1 or later** is required. Packages are not published yet: build from source (below), then copy `obs-vertical.plugin` to `~/Library/Application Support/obs-studio/plugins/`. Restart OBS and open **Docks > Vertical**.

To uninstall, remove the plugin. The vertical canvas and its scenes stay in your scene collection (OBS keeps them, hidden from the main scene list) and come back when the plugin is installed again.

## Documentation

| Document | Content |
|---|---|
| [Architecture](docs/ARCHITECTURE.md) | Layers, canvas, CPU strategy, outputs, persistence, threads |
| [Integration](docs/INTEGRATION.md) | Proc handlers for other plugins (`vertical_get_canvas`, `vertical_link_scene`) |
| [Virtual camera](docs/VIRTUAL-CAMERA.md) | Camera extension design, signing, notarization, Apple Developer steps |

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
