# Architecture

Vertical for OBS adds a second, portrait canvas to OBS 33 and outputs it: several
streaming destinations at once, a recording, a replay buffer and a 9:16 virtual
camera. The design goal is a vertical output that stays **reliable** and costs as
**little CPU** as possible next to the main 16:9 output.

## 1. Layers

```
src/core/   pure C++17, no OBS, no Qt          unit-tested (tests/)
  json            minimal JSON value/parser/writer (settings)
  destination     platforms, URL parsing (rtmp/rtmps/srt), validation, key redaction
  encoders        encoder sharing plan, hardware encoder choice, AAC choice
  canvas-size     presets, size checks, output scaling
  scene-links     main scene -> vertical scene links, dock order, unique names
  render-gate     reference-counted "who needs the picture" (output/preview/vcam)
  recording       recording formats -> OBS output types (hybrid MP4/MOV, MKV, ...)
  settings        everything saved with a scene collection, JSON (de)serialization

src/obs/    libobs + frontend API, UI thread unless noted
  vertical        the canvas, its scenes, linked scenes, persistence, render gate
  outputs         one obs_output per destination, recording, replay, vcam; encoder pool
  hotkeys         frontend hotkeys (streams, record, replay) and their bindings
  api             proc handlers for other plugins (any thread, see INTEGRATION.md)
  virtualcam-mac  raw output that feeds the Vertical Camera extension (macOS)

src/ui/     Qt widgets
  dock            "Vertical" dock: preview, scenes, outputs, status
  preview         obs_display that draws the canvas texture, letterboxed
  settings-dialog canvas, destinations, recording/replay
  destination-dialog  one destination with live validation

camera-extension/   CoreMediaIO camera extension + host app (Swift), see VIRTUAL-CAMERA.md
```

Everything that can be decided without OBS lives in `src/core` and is tested by
`tests/` (no OBS dependency): `cmake -S tests -B build_tests && cmake --build build_tests && ./build_tests/vertical-core-tests`.

## 2. The canvas

* Created with `obs_frontend_add_canvas(name, ovi, ACTIVATE | SCENE_REF)`, so the
  frontend saves it **and its scenes** with the scene collection, and other plugins
  see it (`OBS_FRONTEND_EVENT_CANVAS_ADDED`, `obs_frontend_get_canvases`).
* Our settings (JSON under the `vertical` key of the collection) remember the canvas
  uuid. When a collection loads, the frontend recreates the canvas **without video**;
  we find it by uuid and call `obs_canvas_reset_video` with our size and the main
  video's fps, formats, colour space and scale filter.
* Default 1080x1920. Presets 720x1280, 1440x2560, 2160x3840, 1080x1350, 1080x1080,
  or custom (even, 128..8192). An optional output height downscales in the mix
  (e.g. 1080x1920 canvas encoded at 720x1280). Resizing is refused while an output
  runs (`obs_canvas_reset_video` refuses an active canvas anyway).
* Scenes: `obs_canvas_scene_create`, removal with `obs_source_remove`, rename with
  `obs_source_set_name` (renames inside a non-main canvas are only signalled on the
  canvas, so we listen there). The dock order is ours (saved list, new scenes at the
  end). At least one scene always exists.
* Program: channel 0 of the canvas is the current vertical scene (straight cut).
* Audio: the canvas does **not** use `MIX_AUDIO`. OBS has one audio output; a canvas
  with `MIX_AUDIO` would add the sources of the vertical scenes to the main mix as
  well. Vertical outputs encode the same mix as the main output, on a chosen track
  (1..6). Vertical-only sounds go on a track that the main output does not use.

## 3. CPU strategy

The vertical output must not double the cost of the main output. Five decisions:

1. **Hardware encoders first.** At runtime `obs_enum_encoder_types` lists the video
   encoders; `core::chooseVideoEncoder` picks, in order: Apple VideoToolbox hardware
   (`com.apple.videotoolbox.videoencoder.ave.avc` / `.ave.hevc` on Apple silicon,
   `.gva` on Intel Macs), other hardware encoders (NVENC, AMF, QSV, VA-API on other
   systems), then hardware H.264 when HEVC was asked but only H.264 has hardware, and
   **x264 only when no hardware encoder exists** (logged as a warning, shown in the
   dock). Audio uses CoreAudio AAC on macOS, then FFmpeg AAC.

2. **One encoder per distinct setting, not per output.** The encoder pool is keyed by
   the canonical text of the settings (`EncoderSettings::videoKey()` /
   `audioKey()`). TikTok and YouTube at 6000 kbps / 2 s keyframes share **one** video
   encoder and **one** audio encoder; Instagram at 4000 kbps gets a second video
   encoder but still shares the audio encoder. The recording and the replay buffer
   join the same pool, so a recording at the stream bitrate costs no extra encode.
   An encoder is released when its last output stops. Changing a destination's
   settings while it runs takes effect at its next start (it then joins or creates
   the matching group).

3. **The canvas renders only when someone needs it.** libobs renders every canvas
   mix on every frame. The render gate (`core::RenderGate`) counts three reasons:
   running outputs (streams, recording, replay), the dock preview while visible, and
   the virtual camera. When the gate is idle, channel 0 of the canvas is set to
   nothing: the mix only clears its texture, no scene is drawn and the vertical-only
   sources are not active for this canvas. When a reason appears, the current scene
   goes back on channel 0. Hiding the preview (or the dock) is enough to stop the
   vertical render when nothing is on air.

   Rejected: shrinking the idle canvas to a few pixels with `obs_canvas_reset_video`.
   It would emit `canvas_video_reset` and other plugins (Layouts keeps item
   constraints per canvas size) would re-layout the vertical scenes.

4. **Sources are shared by reference.** A camera shown in a 16:9 scene and in a
   vertical scene is the same `obs_source_t`: one capture, one decode, two
   placements. Only the composition is rendered twice.

5. **No per-frame work in the plugin.** Encoders receive frames straight from the
   canvas `video_t` (`obs_encoder_set_video(enc, obs_canvas_get_video(canvas))`).
   VideoToolbox in OBS 33 takes raw frames, so the mix converts to NV12 on the GPU and
   downloads one frame per mix, shared by all encoders of that canvas. The virtual
   camera reads the same frames (`obs_output_set_media`) and copies each plane once
   into an IOSurface-backed pixel buffer.

Expected extra cost of a vertical output at 1080x1920/30 next to a 1080p main output
on Apple silicon: one extra composite on the GPU, one NV12 download, one hardware
encode per distinct bitrate; CPU stays in the low single digits of percent. To be
measured on the M1 Max and M4 (see the open questions).

## 4. Outputs

| Output | OBS type | Notes |
| --- | --- | --- |
| RTMP / RTMPS destination | `rtmp_output` + private `rtmp_custom` service | TikTok, YouTube, Instagram, Facebook, custom. Reconnect 25 × 2 s. |
| SRT destination | `ffmpeg_mpegts_muxer` + `rtmp_custom` (srt:// server) | Port required; key used as stream id, passphrase in the URL. |
| Recording | `mp4_output` / `mov_output` (hybrid), else `ffmpeg_muxer` | Hybrid MP4 by default; falls back to MKV when hybrid is missing. |
| Replay buffer | `replay_buffer` | Save through its `save` proc, path from `get_last_replay`. |
| Virtual camera | `vertical_virtualcam` (ours) | Raw NV12 into the camera extension's sink stream. |

Each output reports `start`, `stop` (code + last error), `reconnect`,
`reconnect_success`; the handlers hop to the UI thread. Status per destination:
Off, Connecting, Live (elapsed, kbps over the last second, dropped frames),
Reconnecting, Stopping, Error (message kept after the stop). Stream keys are never
logged (`core::redactKey` for diagnostics).

On scene collection change and on exit every output is force-stopped before the
frontend clears the canvases.

## 5. Linked scenes

`core::SceneLinks` maps a main scene name to a vertical scene name (several main
scenes can share one vertical scene). With "Linked scenes" on, a change of the main
program scene switches the vertical canvas to the linked scene; without a link the
vertical output keeps its scene. Renames of main or vertical scenes update the links;
deleting a vertical scene drops its links, deleting a main scene drops its link
(except during a collection switch, when every scene is removed). Links are saved
with the collection and can be set by other plugins (INTEGRATION.md).

## 6. Persistence

Per scene collection, in the collection JSON:

* `vertical`: `core::Settings` (canvas uuid/name/size, scene order, current scene,
  linked mode and links, destinations with their keys, recording, replay, preview
  visibility). Stream keys are stored in clear like OBS stores its own keys.
* `vertical_hotkeys`: bindings of the frontend hotkeys (streams, recording, replay,
  each destination). The "switch to this scene" hotkey is registered on each vertical
  scene source, so OBS saves it with the scene and it survives renames.
* The canvas and its scenes are saved by the frontend itself.

Reading is tolerant: unknown keys are ignored, invalid values fall back to defaults
with a warning in the log, a newer settings version is read as far as possible.

## 7. Threads

UI thread: `Vertical`, `Outputs`, dock and dialogs. Output signals, hotkeys and
global source signals arrive on other threads and are queued to the UI thread with
`QMetaObject::invokeMethod`. The proc handlers (`api.cpp`) read a mutex-protected
snapshot of the canvas and change links under the same mutex. The preview draw
callback runs on the graphics thread with a weak reference to the canvas.
