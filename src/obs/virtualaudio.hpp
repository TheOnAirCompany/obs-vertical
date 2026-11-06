#pragma once

/* "Vertical Audio" (macOS): plays the vertical mix into the hidden "Vertical Audio Feed"
 * device of the Vertical Audio driver (camera-extension/audio-driver), which loops it
 * to the "Vertical Audio" microphone. The feed runs only while an app records from
 * "Vertical Audio" (the device reports it is running somewhere), so it costs nothing
 * otherwise. See docs/VIRTUAL-AUDIO.md. */

#include <cstdint>

namespace vertical::virtualaudio {

struct Stats {
	bool installed = false;    /* the driver's devices exist */
	bool listening = false;    /* an app records from "Vertical Audio" */
	bool feeding = false;      /* OBS plays into the feed device */
	double scheduleMs = 0.0;   /* OBS timestamp -> feed output, without the offset */
	double audioDelayMs = 0.0; /* extra delay from the offset */
	uint64_t underruns = 0;    /* IO cycles that found no audio yet */
	uint64_t chunks = 0;       /* mix chunks received */
	uint32_t bufferFrames = 0; /* IO buffer of the feed device */
};

#ifdef __APPLE__
/* obs_module_load: watch the devices; feed automatically while an app listens. */
void start();
/* OBS_FRONTEND_EVENT_EXIT. */
void shutdown();
/* The vertical track changed: listen to the new mix. UI thread. */
void trackChanged();
/* Feed even when no app listens (self-test, measurements). UI thread. */
void setForced(bool on);
bool isDriverInstalled();
Stats stats();
#else
inline void start() {}
inline void shutdown() {}
inline void trackChanged() {}
inline void setForced(bool) {}
inline bool isDriverInstalled()
{
	return false;
}
inline Stats stats()
{
	return Stats();
}
#endif

} // namespace vertical::virtualaudio
