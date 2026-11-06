# Changelog

## 0.1.0 (in development)

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
- 9:16 virtual camera: output, CoreMediaIO camera extension and host app scaffold (not signed yet).
- English and French.
