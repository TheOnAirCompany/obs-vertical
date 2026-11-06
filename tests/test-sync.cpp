#include "test.hpp"

#include "loopback-ring.hpp"
#include "sync.hpp"
#include "timed-ring.hpp"

#include <cmath>
#include <vector>

using namespace vertical::core;

namespace {

constexpr uint64_t kSec = 1000000000ull;
constexpr uint64_t kT0 = 5000 * kSec; /* some host time */

uint64_t nsOf(int64_t frames)
{
	return (uint64_t)(frames * 1000000000 / 48000);
}

/* Writes `chunks` OBS-like chunks (1024 frames) whose samples are their frame index. */
void writeChunks(TimedRing &r, int64_t firstFrame, int chunks, uint64_t t0 = kT0)
{
	std::vector<float> l(1024), rr(1024);
	for (int c = 0; c < chunks; c++) {
		const int64_t base = firstFrame + c * 1024;
		for (int i = 0; i < 1024; i++)
			l[(size_t)i] = rr[(size_t)i] = (float)(base + i);
		const float *planes[2] = {l.data(), rr.data()};
		r.writePlanar(planes, 1024, t0 + nsOf(base));
	}
}

} // namespace

TEST("sync: shortest audio schedule and paths")
{
	LatencyInputs in;
	in.obsAudioLagMs = 10.0;
	/* lag + chunk (21.33) + feed buffer (5.33) + margin (2) */
	CHECK_NEAR(audioScheduleMs(in), 10.0 + 21.333 + 5.333 + 2.0, 0.01);
	CHECK_NEAR(audioPathMs(in), audioScheduleMs(in) + 2.667, 0.01);
	in.videoLagMs = 50.0;
	CHECK_NEAR(videoPathMs(in), 50.0 + 4.1667, 0.01);
	in.obsAudioLagMs = -5.0; /* clock noise never shortens the path */
	CHECK_NEAR(audioScheduleMs(in), 21.333 + 5.333 + 2.0, 0.01);
}

TEST("sync: default offset = video path - audio path")
{
	LatencyInputs in;
	in.obsAudioLagMs = 0.0;
	in.videoLagMs = 66.7; /* two frames at 30 fps */
	/* 66.7 + 4.17 - (21.33 + 5.33 + 2 + 2.67) = 39.5 -> 40: audio is delayed */
	CHECK_EQ(defaultOffsetMs(in), 40);
	in.obsAudioLagMs = 64.0; /* heavy OBS audio buffering: the image waits */
	CHECK_EQ(defaultOffsetMs(in), -24);
	in.videoLagMs = 5000.0;
	CHECK_EQ(defaultOffsetMs(in), kMaxOffsetMs);
	in.videoLagMs = 0.0;
	in.obsAudioLagMs = 5000.0;
	CHECK_EQ(defaultOffsetMs(in), kMinOffsetMs);
	/* Defaults (nothing measured yet) give a small positive offset. */
	const int d = defaultOffsetMs(LatencyInputs());
	CHECK(d > 0 && d < 40);
}

TEST("sync: plan delays the image in whole frames and the audio by the rest")
{
	SyncPlan p = planSync(30, 30.0);
	CHECK_EQ(p.videoDelayFrames, 0);
	CHECK_NEAR(p.audioDelayMs, 30.0, 1e-9);
	p = planSync(0, 30.0);
	CHECK_EQ(p.videoDelayFrames, 0);
	CHECK_NEAR(p.audioDelayMs, 0.0, 1e-9);
	p = planSync(-10, 30.0); /* one frame (33.3) late image, audio 23.3 later: net image +10 */
	CHECK_EQ(p.videoDelayFrames, 1);
	CHECK_NEAR(p.audioDelayMs, 23.333, 0.01);
	p = planSync(-100, 30.0); /* exactly 3 frames */
	CHECK_EQ(p.videoDelayFrames, 3);
	CHECK_NEAR(p.audioDelayMs, 0.0, 1e-6);
	p = planSync(-50, 60.0);
	CHECK_EQ(p.videoDelayFrames, 3);
	CHECK_NEAR(p.audioDelayMs, 0.0, 0.01);
	p = planSync(-9999, 60.0); /* clamped */
	CHECK_EQ(p.videoDelayFrames, 30);
	p = planSync(-10, 0.0); /* no fps: no video delay */
	CHECK_EQ(p.videoDelayFrames, 0);
	CHECK_NEAR(p.audioDelayMs, 0.0, 1e-9);
	/* net shift = video delay - audio delay = -offset */
	for (int off = -400; off <= 0; off += 7) {
		const SyncPlan q = planSync(off, 30.0);
		CHECK_NEAR(q.videoDelayFrames * 1000.0 / 30.0 - q.audioDelayMs, -off, 1e-6);
		CHECK(q.audioDelayMs >= 0.0 && q.audioDelayMs < 1000.0 / 30.0);
	}
}

TEST("sync: lag tracker rises at once, releases slowly")
{
	LagTracker t(0.5);
	CHECK(!t.valid());
	t.add(10.0);
	CHECK_NEAR(t.value(), 10.0, 1e-9);
	t.add(20.0);
	CHECK_NEAR(t.value(), 20.0, 1e-9);
	t.add(5.0);
	CHECK_NEAR(t.value(), 19.5, 1e-9);
	for (int i = 0; i < 100; i++)
		t.add(5.0);
	CHECK_NEAR(t.value(), 5.0, 1e-9);
	t.reset();
	CHECK(!t.valid());
}

TEST("timed ring: reads the frames stamped from a time")
{
	TimedRing r(8192);
	CHECK_EQ(r.capacity(), 8192u);
	std::vector<float> out(2 * 256);
	/* Nothing written yet: silence, counted as not yet written. */
	TimedRing::ReadResult res = r.read(out.data(), 256, kT0);
	CHECK_EQ(res.notYetWritten, 256u);
	CHECK_EQ(r.writtenEndTs(), 0u);

	writeChunks(r, 0, 4); /* frames 0..4095 */
	CHECK_EQ(r.writtenEndTs(), kT0 + nsOf(4096));
	res = r.read(out.data(), 256, kT0 + nsOf(1000));
	CHECK_EQ(res.filled, 256u);
	CHECK_EQ(out[0], 1000.0f);
	CHECK_EQ(out[1], 1000.0f);
	CHECK_EQ(out[2 * 255], 1255.0f);

	/* A later time reads later frames: the delay is just where we read. */
	res = r.read(out.data(), 256, kT0 + nsOf(1000) + 10 * 1000000); /* +10 ms = 480 frames */
	CHECK_EQ(out[0], 1480.0f);

	/* Past the end: partly filled, the rest is silence. */
	res = r.read(out.data(), 256, kT0 + nsOf(4000));
	CHECK_EQ(res.filled, 96u);
	CHECK_EQ(res.notYetWritten, 160u);
	CHECK_EQ(out[2 * 95], 4095.0f);
	CHECK_EQ(out[2 * 96], 0.0f);

	/* Before the start. */
	res = r.read(out.data(), 256, kT0 - nsOf(100));
	CHECK_EQ(res.tooOld, 100u);
	CHECK_EQ(res.filled, 156u);
	CHECK_EQ(out[2 * 100], 0.0f + 0.0f);
}

TEST("timed ring: overwritten data is too old, never stale")
{
	TimedRing r(8192);
	writeChunks(r, 0, 20); /* 20480 frames, ring keeps the last 8192 */
	std::vector<float> out(2 * 64);
	const auto res = r.read(out.data(), 64, kT0 + nsOf(1000));
	CHECK_EQ(res.tooOld, 64u);
	/* The newest frames, minus the guard, are readable. */
	const int64_t newest = 20480 - 64;
	const auto ok = r.read(out.data(), 64, kT0 + nsOf(newest));
	CHECK_EQ(ok.filled, 64u);
	CHECK_EQ(out[0], (float)newest);
}

TEST("timed ring: re-anchors on a timestamp jump, tolerates rounding")
{
	TimedRing r(8192);
	writeChunks(r, 0, 2);
	/* Next chunk 1 ms late: within tolerance, stays contiguous. */
	std::vector<float> l(1024, 7.0f), rr(1024, 7.0f);
	const float *planes[2] = {l.data(), rr.data()};
	r.writePlanar(planes, 1024, kT0 + nsOf(2048) + 1000000);
	std::vector<float> out(2 * 4);
	r.read(out.data(), 4, kT0 + nsOf(2048));
	CHECK_EQ(out[0], 7.0f);
	/* A jump of one second: the ring follows the new timeline. */
	const uint64_t later = kT0 + 10 * kSec;
	std::vector<float> m(1024, 9.0f);
	const float *planes2[2] = {m.data(), m.data()};
	r.writePlanar(planes2, 1024, later);
	const auto res = r.read(out.data(), 4, later + nsOf(10));
	CHECK_EQ(res.filled, 4u);
	CHECK_EQ(out[0], 9.0f);
	CHECK_EQ(r.writtenEndTs(), later + nsOf(1024));
	r.reset();
	CHECK_EQ(r.writtenEndTs(), 0u);
}

TEST("timed ring: frame arithmetic stays exact on long sessions")
{
	TimedRing r(1024);
	CHECK_EQ(r.framesBetween(0, kSec), 48000);
	CHECK_EQ(r.framesBetween(kSec, 0), -48000);
	CHECK_EQ(r.framesBetween(0, 100ull * 3600 * kSec), (int64_t)48000 * 3600 * 100);
	CHECK_EQ(r.nsForFrames(48000), kSec);
	CHECK_EQ(r.framesBetween(0, r.nsForFrames(123457)), 123457);
	CHECK_EQ(r.nsForFrames((int64_t)48000 * 3600 * 100), 100ull * 3600 * kSec);
}

TEST("loopback ring: reads what was written at the same sample time, once")
{
	LoopbackRing ring(1000); /* rounded to 1024 */
	CHECK_EQ(ring.capacity(), 1024u);
	std::vector<float> in(2 * 300), out(2 * 300);
	for (size_t i = 0; i < 300; i++)
		in[2 * i] = in[2 * i + 1] = (float)(i + 1);
	/* Wraps around the end of the ring. */
	ring.write(900, in.data(), 300);
	ring.readAndClear(900, out.data(), 300);
	CHECK(out == in);
	/* Read twice: silence the second time. */
	ring.readAndClear(900, out.data(), 300);
	CHECK_EQ(out[0], 0.0f);
	CHECK_EQ(out[2 * 299 + 1], 0.0f);
	/* Offset reads see the right frames. */
	ring.write(-50, in.data(), 300); /* negative sample times work too */
	ring.readAndClear(0, out.data(), 10);
	CHECK_EQ(out[0], 51.0f);
	ring.clear();
	ring.readAndClear(10, out.data(), 10);
	CHECK_EQ(out[0], 0.0f);
	/* Larger than the ring: the rest is silence. */
	std::vector<float> big(2 * 2000, 1.0f), bigOut(2 * 2000, 5.0f);
	ring.write(0, big.data(), 2000);
	ring.readAndClear(0, bigOut.data(), 2000);
	CHECK_EQ(bigOut[2 * 1999], 0.0f);
}

TEST("sync: automatic offset compensates only a real mismatch")
{
	/* Aligned within the threshold: nothing added, latency stays low. */
	CHECK_EQ(autoOffsetMs(0, 0), 0);
	CHECK_EQ(autoOffsetMs(14, 0), 0);
	CHECK_EQ(autoOffsetMs(-14, 0), 0);
	/* Above it: the whole mismatch, in either direction. */
	CHECK_EQ(autoOffsetMs(15, 0), 15);
	CHECK_EQ(autoOffsetMs(-21, 0), -21);
	/* Compensating: small moves are ignored, larger ones followed. */
	CHECK_EQ(autoOffsetMs(-23, -21), -21);
	CHECK_EQ(autoOffsetMs(-27, -21), -27);
	/* It keeps compensating down to threshold - hysteresis, then stops. */
	CHECK_EQ(autoOffsetMs(15, 18), 18);
	CHECK_EQ(autoOffsetMs(12, 18), 12);
	CHECK_EQ(autoOffsetMs(11, 18), 11);
	CHECK_EQ(autoOffsetMs(9, 18), 0);
	CHECK_EQ(autoOffsetMs(-9, -18), 0);
	/* Clamped like the setting. */
	CHECK_EQ(autoOffsetMs(9000, 0), kMaxOffsetMs);
	/* The smallest added latency: only the faster path waits, by the difference. */
	const SyncPlan p = planSync(autoOffsetMs(-21, 0), 30.0);
	CHECK_EQ(p.videoDelayFrames, 1);
	CHECK_NEAR(p.audioDelayMs, 33.333 - 21.0, 0.01);
	const SyncPlan q = planSync(autoOffsetMs(20, 0), 30.0);
	CHECK_EQ(q.videoDelayFrames, 0);
	CHECK_NEAR(q.audioDelayMs, 20.0, 1e-9);
}

TEST("sync: lag average follows the typical value")
{
	LagAverage a(0.5);
	CHECK(!a.valid());
	a.add(40.0);
	CHECK_NEAR(a.value(), 40.0, 1e-9);
	a.add(60.0);
	CHECK_NEAR(a.value(), 50.0, 1e-9);
	for (int i = 0; i < 50; i++)
		a.add(30.0);
	CHECK_NEAR(a.value(), 30.0, 1e-6);
	a.reset();
	CHECK(!a.valid());
}
