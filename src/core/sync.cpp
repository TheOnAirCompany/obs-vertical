#include "sync.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>

namespace vertical::core {

double audioScheduleMs(const LatencyInputs &in)
{
	return std::max(0.0, in.obsAudioLagMs) + framesToMs(kObsChunkFrames) + framesToMs(kFeedBufferFrames) +
	       kScheduleMarginMs;
}

double audioPathMs(const LatencyInputs &in)
{
	return audioScheduleMs(in) + framesToMs(kLoopbackFrames);
}

double videoPathMs(const LatencyInputs &in)
{
	return std::max(0.0, in.videoLagMs) + kSinkWaitMs;
}

int clampOffsetMs(int ms)
{
	return std::clamp(ms, kMinOffsetMs, kMaxOffsetMs);
}

int defaultOffsetMs(const LatencyInputs &in)
{
	return clampOffsetMs((int)std::lround(videoPathMs(in) - audioPathMs(in)));
}

int autoOffsetMs(int predictedMs, int currentMs, int thresholdMs, int hysteresisMs)
{
	predictedMs = clampOffsetMs(predictedMs);
	const int magnitude = std::abs(predictedMs);
	if (currentMs == 0)
		return magnitude >= thresholdMs ? predictedMs : 0;
	/* Compensating: stop below the threshold minus the hysteresis, else follow. */
	if (magnitude < thresholdMs - hysteresisMs)
		return 0;
	return std::abs(predictedMs - currentMs) >= hysteresisMs ? predictedMs : currentMs;
}

SyncPlan planSync(int offsetMs, double fps)
{
	SyncPlan p;
	offsetMs = clampOffsetMs(offsetMs);
	if (offsetMs >= 0 || !(fps > 0.0)) {
		p.audioDelayMs = std::max(0, offsetMs);
		return p;
	}
	const double frameMs = 1000.0 / fps;
	/* Small epsilon: an offset of exactly k frames must not round up to k + 1. */
	p.videoDelayFrames = (int)std::ceil(-offsetMs / frameMs - 1e-9);
	p.audioDelayMs = std::max(0.0, p.videoDelayFrames * frameMs + offsetMs);
	return p;
}

} // namespace vertical::core
