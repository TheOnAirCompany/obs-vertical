#pragma once

/* Hotkeys of the vertical output (Settings > Hotkeys):
 *  - start/stop all streams, start/stop each destination,
 *  - start/stop recording, start/stop replay buffer, save replay,
 *  - select a vertical scene (registered on each scene, see Vertical).
 * Bindings of the frontend hotkeys are saved with the scene collection. */

#include <obs.h>

namespace vertical::hotkeys {

void registerStatic();
/* Registers/unregisters the per-destination pairs to match the settings. */
void syncDestinations();
void load(obs_data_t *collectionData);
void save(obs_data_t *collectionData);
void unregisterAll();

} // namespace vertical::hotkeys
