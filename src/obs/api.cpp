#include "obs/api.hpp"

#include "obs/vertical.hpp"

#include <obs.h>

#include <atomic>
#include <string>

namespace vertical::api {

namespace {

std::atomic<bool> g_alive{false};

/* void vertical_get_canvas(out bool ready, out string uuid, out string name,
 *                          out int width, out int height, out int api_version) */
void getCanvas(void *, calldata_t *cd)
{
	calldata_set_int(cd, "api_version", kApiVersion);
	if (!g_alive) {
		calldata_set_bool(cd, "ready", false);
		return;
	}
	const Vertical::ApiCanvas c = Vertical::instance().apiCanvas();
	calldata_set_bool(cd, "ready", c.ready);
	calldata_set_string(cd, "uuid", c.uuid.c_str());
	calldata_set_string(cd, "name", c.name.c_str());
	calldata_set_int(cd, "width", c.width);
	calldata_set_int(cd, "height", c.height);
}

/* void vertical_link_scene(in string main_scene, in string vertical_scene,
 *                          out bool success, out string error)
 * An empty vertical_scene removes the link of main_scene. */
void linkScene(void *, calldata_t *cd)
{
	if (!g_alive) {
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "error", "plugin unloaded");
		return;
	}
	const char *main = calldata_string(cd, "main_scene");
	const char *vert = calldata_string(cd, "vertical_scene");
	std::string error;
	const bool ok = Vertical::instance().apiLinkScene(main ? main : "", vert ? vert : "", &error);
	calldata_set_bool(cd, "success", ok);
	calldata_set_string(cd, "error", error.c_str());
}

} // namespace

void registerProcs()
{
	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph,
			 "void vertical_get_canvas(out bool ready, out string uuid, out string name, out int width, "
			 "out int height, out int api_version)",
			 getCanvas, nullptr);
	proc_handler_add(ph,
			 "void vertical_link_scene(in string main_scene, in string vertical_scene, out bool success, "
			 "out string error)",
			 linkScene, nullptr);
	g_alive = true;
}

void shutdown()
{
	g_alive = false;
}

} // namespace vertical::api
