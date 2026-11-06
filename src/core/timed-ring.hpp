#pragma once

/* Lock-free single-producer / single-consumer ring of interleaved float audio where
 * every frame has a timestamp (host clock, ns). The producer (OBS audio thread)
 * writes chunks with the timestamp of their first frame; the consumer (CoreAudio IO
 * thread) asks for the frames stamped from a given time. Reading further back is how
 * the audio gets delayed: no copy, no extra buffer.
 *
 * Consecutive chunks are expected to follow each other in time; when a chunk does
 * not (OBS restarted its audio, a gap), the ring re-anchors on it. Pure C++17. */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vertical::core {

class TimedRing {
public:
	/* capacityFrames is rounded up to a power of two. */
	explicit TimedRing(size_t capacityFrames = 65536, int channels = 2, int sampleRate = 48000)
		: channels_(channels > 0 ? channels : 1),
		  rate_(sampleRate > 0 ? sampleRate : 48000)
	{
		size_t cap = 1;
		while (cap < capacityFrames)
			cap <<= 1;
		capacity_ = cap;
		data_.assign(capacity_ * (size_t)channels_, 0.0f);
	}

	size_t capacity() const { return capacity_; }
	int channels() const { return channels_; }
	int sampleRate() const { return rate_; }

	/* A jump larger than this between the expected and the given timestamp re-anchors. */
	static constexpr int64_t kJumpToleranceNs = 2000000;
	/* Frames the reader keeps away from the oldest data (the writer may be filling it). */
	static constexpr int64_t kGuardFrames = 4096;

	/* ---- producer ---- */

	/* planes: one pointer per channel (planar), frames each; ts of the first frame. */
	void writePlanar(const float *const *planes, size_t frames, uint64_t ts)
	{
		const int64_t pos = beginWrite(ts);
		for (size_t i = 0; i < frames; i++) {
			float *dst =
				&data_[(size_t)((pos + (int64_t)i) & (int64_t)(capacity_ - 1)) * (size_t)channels_];
			for (int c = 0; c < channels_; c++)
				dst[c] = planes[c] ? planes[c][i] : 0.0f;
		}
		writePos_.store(pos + (int64_t)frames, std::memory_order_release);
	}

	/* Interleaved input. */
	void writeInterleaved(const float *frames, size_t count, uint64_t ts)
	{
		const int64_t pos = beginWrite(ts);
		for (size_t i = 0; i < count; i++)
			std::memcpy(&data_[(size_t)((pos + (int64_t)i) & (int64_t)(capacity_ - 1)) * (size_t)channels_],
				    frames + i * (size_t)channels_, sizeof(float) * (size_t)channels_);
		writePos_.store(pos + (int64_t)count, std::memory_order_release);
	}

	/* ---- consumer ---- */

	struct ReadResult {
		size_t filled = 0;        /* frames copied from the ring */
		size_t notYetWritten = 0; /* asked too early: the audio has not arrived (underrun) */
		size_t tooOld = 0;        /* asked too late: already overwritten */
	};

	/* Fills `out` (interleaved, frames * channels) with the frames stamped from ts on;
	 * frames that are not available are zeros. */
	ReadResult read(float *out, size_t frames, uint64_t ts) const
	{
		ReadResult r;
		int64_t anchorPos = 0;
		uint64_t anchorTs = 0;
		if (!anchor(anchorPos, anchorTs)) {
			std::memset(out, 0, sizeof(float) * frames * (size_t)channels_);
			r.notYetWritten = frames;
			return r;
		}
		const int64_t start = anchorPos + framesBetween(anchorTs, ts);
		const int64_t written = writePos_.load(std::memory_order_acquire);
		const int64_t oldest = written - (int64_t)capacity_ + kGuardFrames;
		for (size_t i = 0; i < frames; i++) {
			const int64_t p = start + (int64_t)i;
			float *dst = out + i * (size_t)channels_;
			if (p >= written) {
				std::memset(dst, 0, sizeof(float) * (size_t)channels_);
				r.notYetWritten++;
			} else if (p < oldest || p < 0) {
				std::memset(dst, 0, sizeof(float) * (size_t)channels_);
				r.tooOld++;
			} else {
				std::memcpy(dst, &data_[(size_t)(p & (int64_t)(capacity_ - 1)) * (size_t)channels_],
					    sizeof(float) * (size_t)channels_);
				r.filled++;
			}
		}
		/* The writer may have lapped us while we copied: drop what it overwrote. */
		const int64_t after = writePos_.load(std::memory_order_acquire) - (int64_t)capacity_;
		for (size_t i = 0; i < frames; i++) {
			const int64_t p = start + (int64_t)i;
			if (p >= oldest && p < written && p < after) {
				std::memset(out + i * (size_t)channels_, 0, sizeof(float) * (size_t)channels_);
				r.filled--;
				r.tooOld++;
			}
		}
		return r;
	}

	/* Timestamp just after the last frame written; 0 before the first write. */
	uint64_t writtenEndTs() const
	{
		int64_t anchorPos = 0;
		uint64_t anchorTs = 0;
		if (!anchor(anchorPos, anchorTs))
			return 0;
		const int64_t written = writePos_.load(std::memory_order_acquire);
		return anchorTs + nsForFrames(written - anchorPos);
	}

	/* Producer side only, while no reader runs. */
	void reset()
	{
		writePos_.store(0, std::memory_order_relaxed);
		seq_.store(0, std::memory_order_release);
		hasAnchor_ = false;
	}

	int64_t framesBetween(uint64_t from, uint64_t to) const
	{
		const int64_t d = (int64_t)(to - from); /* signed */
		const int64_t a = d >= 0 ? d : -d;
		/* Split to stay in range for long sessions; round to the nearest frame. */
		const int64_t frames = (a / 1000000000) * rate_ + ((a % 1000000000) * rate_ + 500000000) / 1000000000;
		return d >= 0 ? frames : -frames;
	}
	uint64_t nsForFrames(int64_t frames) const
	{
		const int64_t a = frames >= 0 ? frames : -frames;
		const int64_t ns = (a / rate_) * 1000000000 + (a % rate_) * 1000000000 / rate_;
		return (uint64_t)(frames >= 0 ? ns : -ns);
	}

private:
	int64_t beginWrite(uint64_t ts)
	{
		const int64_t pos = writePos_.load(std::memory_order_relaxed);
		bool reanchor = !hasAnchor_;
		if (!reanchor) {
			const uint64_t expected = anchorTsW_ + nsForFrames(pos - anchorPosW_);
			const int64_t diff = (int64_t)(ts - expected);
			reanchor = diff > kJumpToleranceNs || diff < -kJumpToleranceNs;
		}
		if (reanchor) {
			hasAnchor_ = true;
			anchorPosW_ = pos;
			anchorTsW_ = ts;
			/* Seqlock: odd while the anchor changes. */
			const uint32_t s = seq_.load(std::memory_order_relaxed);
			seq_.store(s + 1, std::memory_order_release);
			std::atomic_thread_fence(std::memory_order_release);
			anchorPos_.store(pos, std::memory_order_relaxed);
			anchorTs_.store(ts, std::memory_order_relaxed);
			seq_.store(s + 2, std::memory_order_release);
		}
		return pos;
	}

	bool anchor(int64_t &pos, uint64_t &ts) const
	{
		for (int attempt = 0; attempt < 64; attempt++) {
			const uint32_t s1 = seq_.load(std::memory_order_acquire);
			if (s1 == 0)
				return false;
			if (s1 & 1u)
				continue;
			pos = anchorPos_.load(std::memory_order_relaxed);
			ts = anchorTs_.load(std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);
			if (seq_.load(std::memory_order_relaxed) == s1)
				return true;
		}
		return false;
	}

	int channels_;
	int rate_;
	size_t capacity_ = 0;
	std::vector<float> data_;

	std::atomic<int64_t> writePos_{0};
	std::atomic<uint32_t> seq_{0};
	std::atomic<int64_t> anchorPos_{0};
	std::atomic<uint64_t> anchorTs_{0};

	/* Producer-only copies of the anchor. */
	bool hasAnchor_ = false;
	int64_t anchorPosW_ = 0;
	uint64_t anchorTsW_ = 0;
};

} // namespace vertical::core
