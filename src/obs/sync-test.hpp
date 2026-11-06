#pragma once

/* "Sync test": a white flash on the vertical canvas and a 1 kHz beep on the vertical
 * track, stamped with the same OBS time, so the Vertical Camera + Vertical Audio pair
 * (or any vertical output) can be checked by eye and ear, or measured with
 * camera-extension/tools (vertical-sync-measure). Nothing is added to the user's scenes:
 * the flash is a private source on channel 1 of the vertical canvas, the beep a private
 * source on a free channel of the main view, on the vertical track only. */

#include <cstdint>

namespace vertical::synctest {

/* Registers the two private source types. obs_module_load. */
void registerSources();

/* Starts a test; the flash and the beep happen kLeadMs later. Returns the OBS time
 * (host clock, ns) of the event, 0 when the canvas is not ready. UI thread. */
uint64_t trigger();
constexpr int kLeadMs = 300;
constexpr int kFlashMs = 100;
constexpr int kBeepMs = 100;
constexpr int kBeepHz = 1000;

/* OBS time of the last test, 0 if none. */
uint64_t lastEventNs();
/* Number of frames the flash source drew white (self-test). */
uint64_t flashFramesDrawn();

/* macOS: also run a test when the Darwin notification
 * "company.theonair.vertical.sync-test" is posted (by vertical-sync-measure). Only while
 * enabled: the Vertical settings dialog is open, or OBS runs with VERTICAL_SYNC_REMOTE=1.
 * Reference-counted: every `true` needs its `false`. */
void setRemoteTrigger(bool on);

/* OBS_FRONTEND_EVENT_EXIT: drops the sources. */
void shutdown();

} // namespace vertical::synctest
