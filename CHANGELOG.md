# Changelog

## 0.3.0 — unreleased

Vertical audio.

- **Vertical Audio** (macOS): a microphone that carries the vertical mix in real time, for TikTok LIVE Studio and any app. Our own audio driver (AudioServerPlugIn), installed with the camera in one package, "Vertical Camera & Audio" (administrator password; the sound stops for about a second while coreaudiod restarts). It shows in microphone lists only, never as a speaker. The plugin feeds it only while an app listens.
- **Vertical mix**: one OBS mixer track managed by Vertical (the first free one from track 6, shown in Settings > Audio): every source on track 1 is on it unless unticked in Settings > Audio ("sent to vertical"; new sources are sent), kept up to date live. Mute and volume in the OBS mixer apply to it. Vertical's streams, recording and replay buffer encode it (destination audio track "Vertical mix" by default; settings saved by 0.2 on track 1 move to it).
- **Audio/video offset** for the Vertical Camera + Vertical Audio pair (Settings > Audio): positive delays the audio, negative delays the image; the default comes from the measured latencies. No extra CPU: the audio is read further back in its ring, the image is held as whole frames without copy.
- **Sync test** (Settings > Audio): a white flash and a beep at the same instant on the vertical outputs.
- **Output meter** of the vertical mix: a thin stereo bar under the dock preview, in Studio and Compact, with the colours of the OBS mixer.
- Self-test: groups `audio`, `vaudio`, `audioshots` (see docs/ARCHITECTURE.md); macOS driver host test in the core tests.
- `vertical-sync-measure` (development tool): measures the audio/video offset and the latencies of the pair in a real app.

## 0.2.0 — unreleased

Redesigned dock.

- Two layouts: Studio (two columns) and Compact (one column), chosen by the dock width; the ⋯ menu can force one.
- Header with the canvas size and frame rate, the Linked scenes switch and the ⋯ menu.
- Preview with a LIVE pill, the aspect tag and a red frame while a destination is live; collapse button; double-click opens a detached preview window.
- Scene list: program scene in red with a PROGRAM badge, drag handle, link icon on each row with a menu of the 16:9 scenes (checkmarks, several links per vertical scene).
- Destinations with platform tiles, status lines from the output engine (connecting, live with uptime and bitrate, reconnecting with the attempt number, error, stopped), a settings button (opens the destination) before ▶ "Go live on …" / ■ "Stop", ⋯ (edit, duplicate, delete), Start all / Stop all in the section header, add a destination from the dock.
- The platform follows the server: a TikTok, YouTube, Instagram or Facebook server (or srt://) selects its platform in the destination dialog, and the tile shows the platform of the server (a TikTok destination no longer shows the Instagram tile).
- Recording, replay buffer and virtual camera tabs in Studio (large Stop Recording button and timer), three lines in Compact; virtual camera status (not installed, waiting for approval, activated, running) with a Beta badge.
- Status bar: outputs live, CPU, version and settings; a small amber "Software encoder (CPU)" warning only when the vertical video is encoded by the CPU (x264), nothing when a hardware encoder is used.
- Colours follow the OBS theme; status colours are fixed.
- Fix: the order of the vertical scenes was not kept in the scene collection (OBS drops bare strings in the arrays of its settings); it is saved as objects now, older files are still read.
- Self-test: the link menu is opened straight from the dock and used at once (a focus change of the test OBS closed it during the test); Linked scenes switching is checked.
- Self-test: dock group (layouts, resize, scene click, link menu, recording tab, destination rows: tiles, settings button before ▶, dialog, add TikTok from the dock) and destination rows during real streams.

## 0.1.0 — 2026-11-06

First version.

- Vertical canvas (OBS 33 canvas API), 1080 × 1920 by default, presets and custom sizes, optional output downscale; saved with the scene collection.
- Vertical dock: live preview, vertical scenes (add, remove, rename, reorder, put on air), outputs and status.
- Linked scenes: a 16:9 scene switches its linked vertical scene.
- Streaming to several destinations at once (TikTok, YouTube, Instagram, Facebook, custom RTMP/RTMPS, SRT) with URL and key validation, per-destination start/stop, reconnection and status.
- Encoder pool: identical settings share one video encoder and one audio encoder; hardware encoders first (VideoToolbox on macOS), x264 only as a fallback.
- Recording (hybrid MP4/MOV, MKV, MP4, MOV) and replay buffer.
- Render gate: the vertical scene is rendered only while an output, the preview or the virtual camera needs it.
- Hotkeys for streams, each destination, recording, replay and vertical scenes.
- Proc handlers `vertical_get_canvas` and `vertical_link_scene` for other plugins.
- 9:16 virtual camera: output, CoreMediaIO camera extension and its host app, shipped as a separate package (`obs-vertical-camera`) that installs Vertical Camera.app into /Applications.
- macOS packages signed (Developer ID) and notarized.
- English and French.
