#pragma once

/* Level meter math for the vertical mix, with the scale and ballistics of OBS's own
 * mixer meters (frontend/components/VolumeMeter.cpp): -60..0 dB on a linear dB
 * scale, nominal below -20 dB, warning from -20 dB, error from -9 dB, peak decay
 * 11.76 dB/s. Pure C++17. */

#include <cstddef>

namespace vertical::core {

constexpr float kMeterMinDb = -60.0f;
constexpr float kMeterWarningDb = -20.0f;
constexpr float kMeterErrorDb = -9.0f;
constexpr float kMeterDecayDbPerSec = 11.76f;
constexpr float kMeterSilenceDb = -1000.0f; /* "no signal" */

/* Linear amplitude -> dBFS; 0 gives kMeterSilenceDb. */
float mulToDb(float mul);

/* Peak (largest absolute sample) of n samples; 0 for none. */
float peakOf(const float *samples, size_t n);

/* Position of a level on the meter: 0 at kMeterMinDb or below, 1 at 0 dB or above. */
float meterFraction(float db, float minDb = kMeterMinDb);

enum class MeterZone { Nominal, Warning, Error };
MeterZone meterZone(float db);

/* Displayed level of one channel: jumps up to a new peak at once, falls back at the
 * decay rate. */
class MeterBallistics {
public:
	/* peakDb: peak of the audio received since the last call; dtSec: time since then. */
	float update(float peakDb, double dtSec, float decayDbPerSec = kMeterDecayDbPerSec);
	float level() const { return level_; }
	void reset() { level_ = kMeterSilenceDb; }

private:
	float level_ = kMeterSilenceDb;
};

} // namespace vertical::core
