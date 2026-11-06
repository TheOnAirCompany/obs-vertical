# Integration with other plugins

Vertical for OBS exposes a small, stable interface on the **global proc handler**
(`obs_get_proc_handler()`), so other plugins (Layouts for OBS first) can find the
vertical canvas and link scenes without linking against this plugin. The procs are
registered in `obs_module_load`; call them after `OBS_FRONTEND_EVENT_FINISHED_LOADING`
(the canvas of a collection exists once the collection is loaded).

All procs are safe to call from any thread. They never block on the UI thread.

## `vertical_get_canvas`

```
void vertical_get_canvas(out bool ready, out string uuid, out string name,
                         out int width, out int height, out int api_version)
```

| Field | Meaning |
| --- | --- |
| `ready` | false when no vertical canvas exists yet (no collection loaded, plugin unloading) |
| `uuid` | canvas uuid: `obs_get_canvas_by_uuid(uuid)` returns a strong reference |
| `name` | canvas name as shown by OBS (default "Vertical") |
| `width`, `height` | base size of the canvas (default 1080 x 1920) |
| `api_version` | 1 for this interface; always set, even when not ready |

```c
calldata_t cd = {0};
if (proc_handler_call(obs_get_proc_handler(), "vertical_get_canvas", &cd) &&
    calldata_bool(&cd, "ready")) {
	obs_canvas_t *canvas = obs_get_canvas_by_uuid(calldata_string(&cd, "uuid"));
	/* ... create or edit scenes with obs_canvas_scene_create / obs_canvas_enum_scenes ... */
	obs_canvas_release(canvas);
}
calldata_free(&cd);
```

`proc_handler_call` returns false when Vertical for OBS is not installed.

## `vertical_link_scene`

```
void vertical_link_scene(in string main_scene, in string vertical_scene,
                         out bool success, out string error)
```

Links a main (16:9) scene, by name, to a vertical scene, by name. When the user has
"Linked scenes" on, switching the main program to `main_scene` puts `vertical_scene`
on the vertical output. An empty `vertical_scene` removes the link of `main_scene`.
A main scene has at most one link; several main scenes may link to the same vertical
scene. The link is saved with the scene collection and follows renames.

Fails (`success = false`, `error` set) when the canvas is not ready, the main scene
name is empty, or no vertical scene has that name.

```c
calldata_t cd = {0};
calldata_set_string(&cd, "main_scene", "Interview");
calldata_set_string(&cd, "vertical_scene", "Interview ▯");
proc_handler_call(obs_get_proc_handler(), "vertical_link_scene", &cd);
if (!calldata_bool(&cd, "success"))
	blog(LOG_WARNING, "link failed: %s", calldata_string(&cd, "error"));
calldata_free(&cd);
```

The proc does not turn "Linked scenes" on: that stays the operator's choice in the dock.

## Using the canvas directly

What other plugins can rely on:

* The canvas is a frontend canvas (`obs_frontend_get_canvases`), flags
  `ACTIVATE | SCENE_REF`, saved with the collection with its scenes.
* Its size changes only when the user changes it in the Vertical settings, and only
  while no vertical output runs; listen to the canvas `video_reset` signal.
* Channel 0 is owned by Vertical for OBS. It is empty while nothing needs the vertical
  picture (see ARCHITECTURE.md, render gate): do not set it, and do not treat an empty
  channel as an error.
* Scenes created by another plugin in this canvas appear in the dock (at the end of
  the list) and can be put on air like any other vertical scene.

## Layouts for OBS

Layouts' dual output (branch `dev/0.6-dual-output`, `docs/DUAL-OUTPUT.md`) looks for
a portrait canvas in this order: the uuid it recorded, a portrait frontend canvas
whose name contains "vertical", any portrait frontend canvas. Vertical's canvas
matches the second rule; using `vertical_get_canvas` first makes it exact. After
building the twin of a 16:9 scene, Layouts can call `vertical_link_scene` so that a
CUT on the main output also switches the vertical one when linked mode is on.

## Versioning

`api_version` increases when a proc changes in an incompatible way. New procs or new
`out` fields do not change it.
