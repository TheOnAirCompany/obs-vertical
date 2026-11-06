#include "meter.hpp"

#include <algorithm>
#include <cmath>

namespace vertical::core {

float mulToDb(float mul)
{
	if (!(mul > 0.0f))
		return kMeterSilenceDb;
	return std::max(kMeterSilenceDb, 20.0f * std::log10(mul));
}

float peakOf(const float *samples, size_t n)
{
	float peak = 0.0f;
	if (!samples)
		return peak;
	for (size_t i = 0; i < n; i++)
		peak = std::max(peak, std::fabs(samples[i]));
	return peak;
}

float meterFraction(float db, float minDb)
{
	if (db <= minDb)
		return 0.0f;
	if (db >= 0.0f)
		return 1.0f;
	return (db - minDb) / -minDb;
}

MeterZone meterZone(float db)
{
	if (db >= kMeterErrorDb)
		return MeterZone::Error;
	if (db >= kMeterWarningDb)
		return MeterZone::Warning;
	return MeterZone::Nominal;
}

float MeterBallistics::update(float peakDb, double dtSec, float decayDbPerSec)
{
	const float decayed = level_ <= kMeterSilenceDb ? kMeterSilenceDb
							: level_ - (float)(std::max(0.0, dtSec) * decayDbPerSec);
	level_ = std::max(peakDb, std::max(decayed, kMeterSilenceDb));
	/* Below the bottom of the scale is silence: no endless decay. */
	if (level_ < kMeterMinDb - 1.0f && peakDb <= kMeterSilenceDb)
		level_ = kMeterSilenceDb;
	return level_;
}

} // namespace vertical::core
