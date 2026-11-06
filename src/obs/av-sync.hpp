#pragma once

/* Audio/video alignment of the Vertical Camera + Vertical Audio pair (see core/sync.hpp
 * for the model). The offset setting is global (OBS user config, [Vertical]), not per
 * scene collection: latencies belong to the machine. The audio feed and the virtual
 * camera report the lags they measure and read the current plan; both from their own
 * threads, through atomics. */

#include "core/sync.hpp"

namespace vertical::avsync {

struct Config {
	bool automatic = true; /* follow the computed default */
	int offsetMs = 0;      /* used when not automatic; positive delays the audio */
};

Config config();
void setConfig(const Config &c); /* saves it and updates the plan */

/* Measured by the feed (OBS audio thread) and the virtual camera (video thread). */
void reportAudioLag(double ms);
void reportVideoLag(double ms);
/* Clears the measurements (outputs stopped for good, tests). */
void resetMeasurements();
bool audioMeasured();
bool videoMeasured();

/* Measured lags where known, typical values otherwise, and the current fps. */
core::LatencyInputs inputs();
int defaultOffsetMs();
/* The offset in force: the default when automatic, else the configured one. */
int effectiveOffsetMs();

/* Recomputes the plan (UI thread; also every second while the pair runs). */
void update();
/* Current plan, any thread. */
double audioDelayMs();
int videoDelayFrames();

} // namespace vertical::avsync
