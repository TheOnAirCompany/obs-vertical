# Changelog

## 0.2.0 — unreleased

Redesigned dock.

- Two layouts: Studio (two columns) and Compact (one column), chosen by the dock width; the ⋯ menu can force one.
- Header with the canvas size and frame rate, the Linked scenes switch and the ⋯ menu.
- Preview with a LIVE pill, the aspect tag and a red frame while a destination is live; collapse button; double-click opens a detached preview window.
- Scene list: program scene in red with a PROGRAM badge, drag handle, link icon on each row with a menu of the 16:9 scenes (checkmarks, several links per vertical scene).
- Destinations with platform tiles, status lines from the output engine (connecting, live with uptime and bitrate, reconnecting with the attempt number, error, stopped), ▶ / ■ and ⋯ (edit, duplicate, delete), Start all / Stop all in the section header, add a destination from the dock.
- Recording, replay buffer and virtual camera tabs in Studio (large Stop Recording button and timer), three lines in Compact; virtual camera status (not installed, waiting for approval, activated, running) with a Beta badge.
- Status bar: shared encoder chip, outputs live, CPU, version and settings.
- Colours follow the OBS theme; status colours are fixed.
- Self-test: dock group (layouts, resize, scene click, link menu, recording tab) and destination rows during real streams.

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
