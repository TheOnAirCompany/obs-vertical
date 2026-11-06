/*
Vertical for OBS
Copyright (C) 2026 The OnAir Company

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/* Plugin side of "Vertical Audio": the vertical mix (OBS raw audio callback on the
 * vertical track) goes into a timed ring; a CoreAudio IO proc on the hidden feed device
 * plays, for each output time, the samples OBS stamped `schedule + offset` earlier. The
 * schedule is the shortest that never runs dry (measured), so the latency is fixed and
 * known; the offset setting only reads further back in the ring. */

#ifdef __APPLE__

#include "obs/virtualaudio.hpp"

#include "core/audio-device-ids.hpp"
#include "core/sync.hpp"
#include "core/timed-ring.hpp"
#include "obs/audio-mix.hpp"
#include "obs/av-sync.hpp"

#include <CoreAudio/CoreAudio.h>
#include <obs.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>

#include <atomic>
#include <cstring>

namespace vertical::virtualaudio {

namespace {

constexpr int kRate = core::kSyncSampleRate;
constexpr int kChannels = 2;
/* The feed plays silence for about a second after a start while it learns how late
 * OBS hands the chunks over; the very first chunks are not counted (OBS just connected
 * us and may deliver them in a burst). */
constexpr uint64_t kIgnoredChunks = 8;
constexpr uint64_t kWarmupChunks = 48;
/* Margin for the wake-up jitter of OBS's audio thread on top of the largest lag seen
 * (provisional: spikes of about 10 ms were measured on a busy Mac). */
constexpr double kJitterMarginMs = 8.0;
/* After an underrun the schedule grows by this much. */
constexpr double kUnderrunStepMs = 3.0;

struct Feed {
	core::TimedRing ring{65536, kChannels, kRate};
	AudioObjectID feedDevice = kAudioObjectUnknown;
	AudioObjectID inputDevice = kAudioObjectUnknown;
	AudioDeviceIOProcID proc = nullptr;
	bool started = false;
	bool feeding = false;
	bool forced = false;
	bool listening = false;
	bool callbackOn = false;
	size_t mix = 0;
	uint32_t bufferFrames = 0;
	QTimer *timer = nullptr;

	/* Audio thread / IO thread. */
	std::atomic<bool> haveAudio{false};
	std::atomic<double> scheduleMs{0.0};
	std::atomic<uint64_t> underruns{0};
	std::atomic<uint64_t> chunks{0};
	/* How far ahead of its output time CoreAudio calls the IO proc: the audio must be in
	 * the ring by then, not by the output time. */
	std::atomic<double> maxLeadMs{0.0};
	double maxLagMs = 0.0; /* OBS audio thread only */
};

Feed g;

AudioObjectID deviceForUID(const char *uid)
{
	CFStringRef s = CFStringCreateWithCString(nullptr, uid, kCFStringEncodingUTF8);
	AudioObjectPropertyAddress a = {kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal,
					kAudioObjectPropertyElementMain};
	AudioObjectID device = kAudioObjectUnknown;
	UInt32 size = sizeof(device);
	const OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(s), &s, &size, &device);
	CFRelease(s);
	return st == noErr ? device : kAudioObjectUnknown;
}

bool runningSomewhere(AudioObjectID device)
{
	if (device == kAudioObjectUnknown)
		return false;
	AudioObjectPropertyAddress a = {kAudioDevicePropertyDeviceIsRunningSomewhere, kAudioObjectPropertyScopeGlobal,
					kAudioObjectPropertyElementMain};
	UInt32 running = 0;
	UInt32 size = sizeof(running);
	return AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, &running) == noErr && running;
}

void addDouble(std::atomic<double> &a, double v)
{
	double cur = a.load();
	while (!a.compare_exchange_weak(cur, cur + v)) {
	}
}

/* ---------------------------------------------------------------- audio threads */

void mixCallback(void *, size_t, struct audio_data *data)
{
	if (!data || !data->frames)
		return;
	const uint64_t now = os_gettime_ns();
	const uint64_t chunk = g.chunks.fetch_add(1) + 1;
	const uint64_t end = data->timestamp + (uint64_t)data->frames * 1000000000ull / kRate;
	const double lagMs = (double)(int64_t)(now - end) / 1e6;
	if (chunk > kIgnoredChunks && lagMs > g.maxLagMs)
		g.maxLagMs = lagMs;
	if (chunk > kIgnoredChunks) {
		core::LatencyInputs in;
		in.obsAudioLagMs = g.maxLagMs;
		const double needed = core::audioScheduleMs(in) + g.maxLeadMs.load() + kJitterMarginMs;
		if (needed > g.scheduleMs.load())
			g.scheduleMs = needed;
		/* What the alignment uses: the lag the schedule actually covers. */
		core::LatencyInputs zero;
		zero.obsAudioLagMs = 0.0;
		avsync::reportAudioLag(g.scheduleMs.load() - core::audioScheduleMs(zero));
	}
	const float *planes[kChannels] = {reinterpret_cast<const float *>(data->data[0]),
					  reinterpret_cast<const float *>(data->data[1] ? data->data[1]
											: data->data[0])};
	g.ring.writePlanar(planes, data->frames, data->timestamp);
	if (chunk > kWarmupChunks)
		g.haveAudio.store(true, std::memory_order_release);
}

void storeMax(std::atomic<double> &a, double v)
{
	double cur = a.load();
	while (v > cur && !a.compare_exchange_weak(cur, v)) {
	}
}

OSStatus ioProc(AudioObjectID, const AudioTimeStamp *now, const AudioBufferList *, const AudioTimeStamp *,
		AudioBufferList *out, const AudioTimeStamp *outputTime, void *)
{
	if (now && outputTime && (now->mFlags & kAudioTimeStampHostTimeValid) &&
	    (outputTime->mFlags & kAudioTimeStampHostTimeValid) && outputTime->mHostTime > now->mHostTime)
		storeMax(g.maxLeadMs,
			 (double)AudioConvertHostTimeToNanos(outputTime->mHostTime - now->mHostTime) / 1e6);
	if (!out || !out->mNumberBuffers)
		return noErr;
	AudioBuffer &buf = out->mBuffers[0];
	const UInt32 channels = buf.mNumberChannels ? buf.mNumberChannels : kChannels;
	const UInt32 frames = buf.mDataByteSize / (channels * sizeof(float));
	auto *dst = static_cast<float *>(buf.mData);
	if (!dst)
		return noErr;
	if (!g.haveAudio.load(std::memory_order_acquire) || channels != kChannels ||
	    !(outputTime->mFlags & kAudioTimeStampHostTimeValid)) {
		std::memset(dst, 0, buf.mDataByteSize);
		return noErr;
	}
	const uint64_t hostNs = AudioConvertHostTimeToNanos(outputTime->mHostTime);
	const double delayMs = g.scheduleMs.load() + avsync::audioDelayMs();
	const uint64_t want = hostNs - (uint64_t)(delayMs * 1e6);
	const core::TimedRing::ReadResult r = g.ring.read(dst, frames, want);
	if (r.notYetWritten) {
		g.underruns.fetch_add(1);
		addDouble(g.scheduleMs, kUnderrunStepMs);
	}
	return noErr;
}

/* ---------------------------------------------------------------- UI thread */

void stopFeed()
{
	if (!g.feeding)
		return;
	if (g.proc) {
		AudioDeviceStop(g.feedDevice, g.proc);
		AudioDeviceDestroyIOProcID(g.feedDevice, g.proc);
		g.proc = nullptr;
	}
	if (g.callbackOn) {
		obs_remove_raw_audio_callback(g.mix, mixCallback, nullptr);
		g.callbackOn = false;
	}
	g.feeding = false;
	g.haveAudio = false;
	if (g.timer)
		g.timer->stop();
	obs_log(LOG_INFO,
		"vertical audio: feed stopped (%llu chunks, %llu underruns, schedule %.1f ms: OBS lag %.1f ms, "
		"IO lead %.1f ms)",
		(unsigned long long)g.chunks.load(), (unsigned long long)g.underruns.load(), g.scheduleMs.load(),
		g.maxLagMs, g.maxLeadMs.load());
}

bool startFeed()
{
	if (g.feeding)
		return true;
	if (g.feedDevice == kAudioObjectUnknown)
		return false;
	g.ring.reset();
	g.haveAudio = false;
	g.scheduleMs = 0.0;
	g.underruns = 0;
	g.chunks = 0;
	g.maxLagMs = 0.0;
	g.maxLeadMs = 0.0;

	/* Smallest stable IO buffer: the feed device is ours alone. */
	AudioObjectPropertyAddress a = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
					kAudioObjectPropertyElementMain};
	UInt32 frames = core::kFeedBufferFrames;
	AudioObjectSetPropertyData(g.feedDevice, &a, 0, nullptr, sizeof(frames), &frames);
	UInt32 size = sizeof(frames);
	if (AudioObjectGetPropertyData(g.feedDevice, &a, 0, nullptr, &size, &frames) == noErr)
		g.bufferFrames = frames;

	struct audio_convert_info conv = {};
	conv.samples_per_sec = kRate;
	conv.format = AUDIO_FORMAT_FLOAT_PLANAR;
	conv.speakers = SPEAKERS_STEREO;
	g.mix = AudioMix::instance().mixIndex();
	obs_add_raw_audio_callback(g.mix, &conv, mixCallback, nullptr);
	g.callbackOn = true;

	OSStatus st = AudioDeviceCreateIOProcID(g.feedDevice, ioProc, nullptr, &g.proc);
	if (st == noErr)
		st = AudioDeviceStart(g.feedDevice, g.proc);
	if (st != noErr) {
		obs_log(LOG_WARNING, "vertical audio: cannot start the feed device (%d)", (int)st);
		g.feeding = true;
		stopFeed();
		return false;
	}
	g.feeding = true;
	if (g.timer)
		g.timer->start();
	avsync::update();
	obs_log(LOG_INFO, "vertical audio: feeding track %zu, IO buffer %u frames", g.mix + 1, g.bufferFrames);
	return true;
}

void evaluate()
{
	const bool want = g.started && g.feedDevice != kAudioObjectUnknown && (g.listening || g.forced);
	if (want)
		startFeed();
	else
		stopFeed();
}

OSStatus runningListener(AudioObjectID, UInt32, const AudioObjectPropertyAddress *, void *)
{
	QMetaObject::invokeMethod(
		qApp,
		[] {
			g.listening = runningSomewhere(g.inputDevice);
			obs_log(LOG_INFO, "vertical audio: an app %s listening", g.listening ? "is" : "is no longer");
			evaluate();
		},
		Qt::QueuedConnection);
	return noErr;
}

const AudioObjectPropertyAddress kRunningAddress = {kAudioDevicePropertyDeviceIsRunningSomewhere,
						    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
const AudioObjectPropertyAddress kDevicesAddress = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
						    kAudioObjectPropertyElementMain};

void resolveDevices()
{
	const AudioObjectID input = deviceForUID(VERTICAL_AUDIO_INPUT_UID);
	const AudioObjectID feed = deviceForUID(VERTICAL_AUDIO_FEED_UID);
	if (input != g.inputDevice) {
		if (g.inputDevice != kAudioObjectUnknown)
			AudioObjectRemovePropertyListener(g.inputDevice, &kRunningAddress, runningListener, nullptr);
		g.inputDevice = input;
		if (input != kAudioObjectUnknown)
			AudioObjectAddPropertyListener(input, &kRunningAddress, runningListener, nullptr);
	}
	if (feed != g.feedDevice) {
		stopFeed();
		g.feedDevice = feed;
	}
	g.listening = runningSomewhere(g.inputDevice);
	evaluate();
}

OSStatus devicesListener(AudioObjectID, UInt32, const AudioObjectPropertyAddress *, void *)
{
	QMetaObject::invokeMethod(qApp, [] { resolveDevices(); }, Qt::QueuedConnection);
	return noErr;
}

} // namespace

void start()
{
	if (g.started)
		return;
	g.started = true;
	g.timer = new QTimer(qApp);
	g.timer->setInterval(1000);
	QObject::connect(g.timer, &QTimer::timeout, [] { avsync::update(); });
	AudioObjectAddPropertyListener(kAudioObjectSystemObject, &kDevicesAddress, devicesListener, nullptr);
	resolveDevices();
	obs_log(LOG_INFO, "vertical audio: driver %s", g.feedDevice != kAudioObjectUnknown ? "found" : "not installed");
}

void shutdown()
{
	if (!g.started)
		return;
	stopFeed();
	AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &kDevicesAddress, devicesListener, nullptr);
	if (g.inputDevice != kAudioObjectUnknown)
		AudioObjectRemovePropertyListener(g.inputDevice, &kRunningAddress, runningListener, nullptr);
	g.inputDevice = g.feedDevice = kAudioObjectUnknown;
	delete g.timer;
	g.timer = nullptr;
	g.started = false;
}

void trackChanged()
{
	if (!g.feeding || g.mix == AudioMix::instance().mixIndex())
		return;
	stopFeed();
	evaluate();
}

void setForced(bool on)
{
	g.forced = on;
	evaluate();
}

bool isDriverInstalled()
{
	return g.started ? g.feedDevice != kAudioObjectUnknown : deviceForUID(VERTICAL_AUDIO_FEED_UID) != 0;
}

Stats stats()
{
	Stats s;
	s.installed = g.feedDevice != kAudioObjectUnknown;
	s.listening = g.listening;
	s.feeding = g.feeding;
	s.scheduleMs = g.scheduleMs.load();
	s.audioDelayMs = avsync::audioDelayMs();
	s.underruns = g.underruns.load();
	s.chunks = g.chunks.load();
	s.bufferFrames = g.bufferFrames;
	return s;
}

} // namespace vertical::virtualaudio

#endif /* __APPLE__ */
