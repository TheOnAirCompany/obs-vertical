#pragma once

/* Public interface for other plugins, on the global proc handler
 * (obs_get_proc_handler()). See docs/INTEGRATION.md. */

namespace vertical::api {

constexpr int kApiVersion = 1;

void registerProcs();
/* Procs cannot be removed from the global handler: after this they answer "not ready". */
void shutdown();

} // namespace vertical::api
