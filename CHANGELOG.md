# Changelog

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
