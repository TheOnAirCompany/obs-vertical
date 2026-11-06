#include "obs/av-sync.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace vertical::avsync {

namespace {

constexpr const char *kSection = "Vertical";
constexpr const char *kKeyAuto = "SyncOffsetAuto";
constexpr const char *kKeyOffset = "SyncOffsetMs";
/* In automatic mode the offset follows the measurements, but only by steps of at
 * least this much: every change moves the audio a little. */
constexpr int kAutoHysteresisMs = 5;

std::mutex g_lock; /* guards the trackers */
core::LagTracker g_audio(0.02);
core::LagTracker g_video(0.05);
std::atomic<double> g_audioLag{-1.0};
std::atomic<double> g_videoLag{-1.0};

std::atomic<double> g_audioDelay{0.0};
std::atomic<int> g_videoFrames{0};
std::atomic<int> g_effective{0};
std::atomic<bool> g_effectiveSet{false};

double currentFps()
{
	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi) && ovi.fps_den)
		return (double)ovi.fps_num / (double)ovi.fps_den;
	return 30.0;
}

} // namespace

Config config()
{
	Config c;
	if (config_t *cfg = obs_frontend_get_user_config()) {
		config_set_default_bool(cfg, kSection, kKeyAuto, true);
		config_set_default_int(cfg, kSection, kKeyOffset, 0);
		c.automatic = config_get_bool(cfg, kSection, kKeyAuto);
		c.offsetMs = core::clampOffsetMs((int)config_get_int(cfg, kSection, kKeyOffset));
	}
	return c;
}

void setConfig(const Config &c)
{
	if (config_t *cfg = obs_frontend_get_user_config()) {
		config_set_bool(cfg, kSection, kKeyAuto, c.automatic);
		config_set_int(cfg, kSection, kKeyOffset, core::clampOffsetMs(c.offsetMs));
		config_save_safe(cfg, "tmp", nullptr);
	}
	g_effectiveSet = false;
	update();
}

void reportAudioLag(double ms)
{
	std::lock_guard<std::mutex> guard(g_lock);
	g_audio.add(ms);
	g_audioLag = g_audio.value();
}

void reportVideoLag(double ms)
{
	std::lock_guard<std::mutex> guard(g_lock);
	g_video.add(ms);
	g_videoLag = g_video.value();
}

void resetMeasurements()
{
	std::lock_guard<std::mutex> guard(g_lock);
	g_audio.reset();
	g_video.reset();
	g_audioLag = -1.0;
	g_videoLag = -1.0;
	g_effectiveSet = false;
}

bool audioMeasured()
{
	return g_audioLag.load() >= 0.0;
}

bool videoMeasured()
{
	return g_videoLag.load() >= 0.0;
}

core::LatencyInputs inputs()
{
	core::LatencyInputs in;
	in.fps = currentFps();
	in.videoLagMs = 2.0 * 1000.0 / in.fps; /* typical: the frame arrives two frames late */
	if (audioMeasured())
		in.obsAudioLagMs = g_audioLag.load();
	if (videoMeasured())
		in.videoLagMs = g_videoLag.load();
	return in;
}

int defaultOffsetMs()
{
	return core::defaultOffsetMs(inputs());
}

int effectiveOffsetMs()
{
	const Config c = config();
	if (!c.automatic)
		return c.offsetMs;
	const int d = defaultOffsetMs();
	if (!g_effectiveSet || std::abs(d - g_effective.load()) >= kAutoHysteresisMs) {
		g_effective = d;
		g_effectiveSet = true;
	}
	return g_effective.load();
}

void update()
{
	const core::SyncPlan p = core::planSync(effectiveOffsetMs(), currentFps());
	g_audioDelay = p.audioDelayMs;
	g_videoFrames = p.videoDelayFrames;
}

double audioDelayMs()
{
	return g_audioDelay.load();
}

int videoDelayFrames()
{
	return g_videoFrames.load();
}

} // namespace vertical::avsync
