#pragma once

/* Audio/video alignment of the Vertical Camera + Vertical Audio pair. Pure C++17.
 *
 * Both paths are measured against OBS timestamps (host clock, ns), which the audio
 * mix and the canvas frames share:
 *
 *   audio  OBS mix chunk (1024 frames) -> plugin ring -> feed device IO buffer
 *          -> driver loopback -> "Vertical Audio" input
 *          The plugin plays the sample stamped t at host time t + schedule, so the
 *          audio path has a fixed length: schedule + loopback.
 *   video  canvas frame stamped t -> raw output callback (t + video lag) -> camera
 *          sink queue -> extension (polls at 120 Hz) -> camera clients
 *
 * The offset setting ("Audio/video offset (ms)") moves one path against the other:
 * positive delays the audio, negative delays the image. Its default is the
 * difference of the two paths, so that both arrive together. */

#include <cstdint>

namespace vertical::core {

constexpr int kSyncSampleRate = 48000;
constexpr int kObsChunkFrames = 1024;  /* AUDIO_OUTPUT_FRAMES: OBS mixes in chunks of 21.3 ms */
constexpr int kFeedBufferFrames = 256; /* IO buffer of the feed device in OBS (5.3 ms) */
constexpr int kLoopbackFrames = 128;   /* driver: the input reads this far behind the output (2.7 ms) */
constexpr double kScheduleMarginMs = 2.0;
constexpr double kSinkWaitMs = 1000.0 / 120.0 / 2.0; /* the extension polls the sink at 120 Hz */
constexpr int kMaxOffsetMs = 500;                    /* video delay memory: 30 frames at 60 fps */
constexpr int kMinOffsetMs = -kMaxOffsetMs;

inline double framesToMs(double frames, int rate = kSyncSampleRate)
{
	return frames * 1000.0 / rate;
}

struct LatencyInputs {
	/* now - end of the chunk when OBS hands a mixed chunk to the plugin. */
	double obsAudioLagMs = 21.3;
	/* now - frame timestamp when the virtual camera output receives a frame. */
	double videoLagMs = 2.0 * 1000.0 / 30.0;
	double fps = 30.0;
};

/* Shortest schedule that never runs out of audio: the whole chunk must have arrived
 * (lag + chunk) and cover one IO buffer, plus a margin for thread wake-ups. */
double audioScheduleMs(const LatencyInputs &in);
/* OBS timestamp -> "Vertical Audio" input, at the shortest schedule. */
double audioPathMs(const LatencyInputs &in);
/* OBS timestamp -> frame handed to camera clients, without delay. */
double videoPathMs(const LatencyInputs &in);
/* videoPath - audioPath, rounded and clamped to [kMinOffsetMs, kMaxOffsetMs]. */
int defaultOffsetMs(const LatencyInputs &in);

int clampOffsetMs(int ms);

/* Automatic offset. Compensating costs latency (one path waits for the other), so
 * nothing is compensated while the predicted mismatch stays under the threshold; above
 * it the faster path waits exactly for the slower one (planSync delays one path only).
 * Hysteresis: once compensating, it stops only below threshold - hysteresis, and moves
 * only by steps of at least hysteresis. */
constexpr int kAutoThresholdMs = 15;
constexpr int kAutoHysteresisMs = 5;
int autoOffsetMs(int predictedMs, int currentMs, int thresholdMs = kAutoThresholdMs,
		 int hysteresisMs = kAutoHysteresisMs);

/* How an offset is applied: the image is delayed by whole frames (frames are queued,
 * never copied), the audio by the rest (reads further back in its ring), so the net
 * shift is exact to the sample. */
struct SyncPlan {
	double audioDelayMs = 0.0; /* added to the shortest schedule */
	int videoDelayFrames = 0;
};
SyncPlan planSync(int offsetMs, double fps);

/* Exponential moving average, for lags whose typical value matters (video). */
class LagAverage {
public:
	explicit LagAverage(double alpha = 0.05) : alpha_(alpha) {}
	void add(double ms)
	{
		value_ = valid_ ? value_ + alpha_ * (ms - value_) : ms;
		valid_ = true;
	}
	bool valid() const { return valid_; }
	double value() const { return value_; }
	void reset()
	{
		valid_ = false;
		value_ = 0.0;
	}

private:
	double alpha_;
	double value_ = 0.0;
	bool valid_ = false;
};

/* Simple running maximum with a slow release, for lags measured on every chunk or
 * frame: rises at once, falls by `releasePerSample` ms per sample. */
class LagTracker {
public:
	explicit LagTracker(double releasePerSample = 0.01) : release_(releasePerSample) {}
	void add(double ms)
	{
		if (!valid_ || ms >= value_)
			value_ = ms;
		else
			value_ = value_ - release_ > ms ? value_ - release_ : ms;
		valid_ = true;
	}
	bool valid() const { return valid_; }
	double value() const { return value_; }
	void reset()
	{
		valid_ = false;
		value_ = 0.0;
	}

private:
	double release_;
	double value_ = 0.0;
	bool valid_ = false;
};

} // namespace vertical::core
