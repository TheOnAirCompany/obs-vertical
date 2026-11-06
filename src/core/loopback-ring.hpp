#pragma once

/* The ring inside the Vertical Audio driver: the feed device (output, written by OBS)
 * and the "Vertical Audio" device (input, read by apps) share one clock, so a frame is
 * addressed by its device sample time. The writer copies at its output sample time;
 * the reader reads `lagFrames` behind its input sample time and clears what it read, so
 * nothing is ever heard twice and silence follows when OBS stops writing.
 * One writer thread and one reader thread (the two IO threads of coreaudiod) touch
 * different regions; the ring is many times larger than an IO buffer. Pure C++17,
 * no allocation after construction. */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vertical::core {

class LoopbackRing {
public:
	explicit LoopbackRing(size_t capacityFrames = 16384, int channels = 2) : channels_(channels > 0 ? channels : 1)
	{
		size_t cap = 1;
		while (cap < capacityFrames)
			cap <<= 1;
		capacity_ = cap;
		data_.assign(capacity_ * (size_t)channels_, 0.0f);
	}

	size_t capacity() const { return capacity_; }
	int channels() const { return channels_; }

	/* Copies `frames` interleaved frames to sample times [sampleTime, sampleTime + frames). */
	void write(int64_t sampleTime, const float *in, size_t frames)
	{
		if (frames > capacity_)
			frames = capacity_;
		forEachSpan(sampleTime, frames, [&](size_t ringFrame, size_t offset, size_t count) {
			std::memcpy(&data_[ringFrame * (size_t)channels_], in + offset * (size_t)channels_,
				    count * (size_t)channels_ * sizeof(float));
		});
	}

	/* Copies the frames of [sampleTime, sampleTime + frames) to `out` and clears them. */
	void readAndClear(int64_t sampleTime, float *out, size_t frames)
	{
		if (frames > capacity_) {
			std::memset(out + capacity_ * (size_t)channels_, 0,
				    (frames - capacity_) * (size_t)channels_ * sizeof(float));
			frames = capacity_;
		}
		forEachSpan(sampleTime, frames, [&](size_t ringFrame, size_t offset, size_t count) {
			float *src = &data_[ringFrame * (size_t)channels_];
			const size_t bytes = count * (size_t)channels_ * sizeof(float);
			std::memcpy(out + offset * (size_t)channels_, src, bytes);
			std::memset(src, 0, bytes);
		});
	}

	void clear() { std::memset(data_.data(), 0, data_.size() * sizeof(float)); }

private:
	template<typename Fn> void forEachSpan(int64_t sampleTime, size_t frames, Fn fn)
	{
		const int64_t mask = (int64_t)capacity_ - 1;
		size_t done = 0;
		while (done < frames) {
			const size_t ringFrame = (size_t)((sampleTime + (int64_t)done) & mask);
			size_t count = capacity_ - ringFrame;
			if (count > frames - done)
				count = frames - done;
			fn(ringFrame, done, count);
			done += count;
		}
	}

	int channels_;
	size_t capacity_ = 0;
	std::vector<float> data_;
};

} // namespace vertical::core
