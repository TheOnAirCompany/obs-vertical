#include "obs/sync-test.hpp"

#include "obs/audio-mix.hpp"
#include "obs/av-sync.hpp"
#include "obs/vertical.hpp"

#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QDir>
#include <QFile>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <notify.h>
#endif

namespace vertical::synctest {

namespace {

constexpr const char *kFlashId = "vertical_sync_flash";
constexpr const char *kBeepId = "vertical_sync_beep";
constexpr uint32_t kFlashChannel = 1; /* vertical canvas: above the scene on channel 0 */
constexpr int kSampleRate = 48000;
constexpr int kFadeOutMs = 5;
constexpr float kBeepAmplitude = 0.5f; /* -6 dBFS */
constexpr double kTwoPi = 6.283185307179586;

std::atomic<uint64_t> g_eventNs{0};
std::atomic<uint64_t> g_flashFrames{0};
std::atomic<uint64_t> g_lastWhiteFrame{0};
std::atomic<uint32_t> g_width{1080};
std::atomic<uint32_t> g_height{1920};

obs_source_t *g_flash = nullptr; /* strong, private */
obs_source_t *g_beep = nullptr;  /* strong, private */
uint32_t g_beepChannel = 0;
bool g_holdsRender = false;
QTimer *g_cleanup = nullptr;

/* ---------------------------------------------------------------- flash source */

const char *flashName(void *)
{
	return "Vertical sync flash";
}

void *flashCreate(obs_data_t *, obs_source_t *source)
{
	return source;
}

void flashDestroy(void *) {}

uint32_t flashWidth(void *)
{
	return g_width.load();
}

uint32_t flashHeight(void *)
{
	return g_height.load();
}

void flashRender(void *, gs_effect_t *)
{
	const uint64_t t0 = g_eventNs.load();
	const uint64_t now = obs_get_video_frame_time();
	if (!t0 || now < t0 || now >= t0 + (uint64_t)kFlashMs * 1000000ull)
		return;
	if (g_lastWhiteFrame.exchange(now) != now)
		g_flashFrames.fetch_add(1);
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 white;
	vec4_set(&white, 1.0f, 1.0f, 1.0f, 1.0f);
	gs_effect_set_vec4(color, &white);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, g_width.load(), g_height.load());
}

/* ---------------------------------------------------------------- beep source */

const char *beepName(void *)
{
	return "Vertical sync beep";
}

void *beepCreate(obs_data_t *, obs_source_t *source)
{
	return source;
}

void beepDestroy(void *) {}

/* ---------------------------------------------------------------- helpers */

uint32_t freeMainChannel()
{
	/* Channels 0..6 belong to the frontend (transition, audio devices). */
	for (uint32_t ch = MAX_CHANNELS - 1; ch > 6; ch--) {
		OBSSourceAutoRelease s = obs_get_output_source(ch);
		if (!s)
			return ch;
	}
	return 0;
}

void writeEventFile(uint64_t t0)
{
	const QString path = QDir::temp().filePath(QStringLiteral("vertical-sync-test.json"));
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return;
	const QString json = QStringLiteral("{\"t0_ns\": %1, \"offset_ms\": %2, \"audio_delay_ms\": %3, "
					    "\"video_delay_frames\": %4, \"track\": %5}\n")
				     .arg(t0)
				     .arg(avsync::effectiveOffsetMs())
				     .arg(avsync::audioDelayMs(), 0, 'f', 2)
				     .arg(avsync::videoDelayFrames())
				     .arg(AudioMix::instance().track());
	f.write(json.toUtf8());
}

void finish()
{
	Vertical &v = Vertical::instance();
	if (v.canvas())
		obs_canvas_set_channel(v.canvas(), kFlashChannel, nullptr);
	if (g_beepChannel) {
		obs_set_output_source(g_beepChannel, nullptr);
		g_beepChannel = 0;
	}
	if (g_holdsRender) {
		v.releaseRender(core::RenderReason::Preview);
		g_holdsRender = false;
	}
}

int g_remoteUsers = 0;
#ifdef __APPLE__
int g_notifyToken = NOTIFY_TOKEN_INVALID;
#endif

} // namespace

void registerSources()
{
	struct obs_source_info flash = {};
	flash.id = kFlashId;
	flash.type = OBS_SOURCE_TYPE_INPUT;
	flash.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
	flash.get_name = flashName;
	flash.create = flashCreate;
	flash.destroy = flashDestroy;
	flash.get_width = flashWidth;
	flash.get_height = flashHeight;
	flash.video_render = flashRender;
	obs_register_source(&flash);

	struct obs_source_info beep = {};
	beep.id = kBeepId;
	beep.type = OBS_SOURCE_TYPE_INPUT;
	beep.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_CAP_DISABLED;
	beep.get_name = beepName;
	beep.create = beepCreate;
	beep.destroy = beepDestroy;
	obs_register_source(&beep);
}

uint64_t trigger()
{
	Vertical &v = Vertical::instance();
	if (!v.ensureCanvas() || !v.canvas())
		return 0;
	if (!g_flash)
		g_flash = obs_source_create_private(kFlashId, "Vertical sync flash", nullptr);
	finish();
	/* A new beep source every time: OBS smooths the timestamps of a source's audio
	 * against what it sent before, which could move this beep off its time. */
	obs_source_release(g_beep);
	g_beep = obs_source_create_private(kBeepId, "Vertical sync beep", nullptr);
	if (!g_flash || !g_beep)
		return 0;

	const core::Size size = v.canvasSize();
	g_width = (uint32_t)size.width;
	g_height = (uint32_t)size.height;

	/* The event falls on a frame boundary kLeadMs ahead: the flash covers whole frames
	 * from that frame on, the beep starts at the same OBS time. */
	const uint64_t interval = obs_get_frame_interval_ns();
	const uint64_t frames = ((uint64_t)kLeadMs * 1000000ull + interval - 1) / interval;
	const uint64_t t0 = obs_get_video_frame_time() + frames * interval;
	g_eventNs = t0;

	/* The picture must be rendered during the test. */
	v.acquireRender(core::RenderReason::Preview);
	g_holdsRender = true;
	obs_canvas_set_channel(v.canvas(), kFlashChannel, g_flash);

	/* The beep: on the vertical track only, mixed as a root of the main view. */
	obs_source_set_audio_mixers(g_beep, core::trackBit(AudioMix::instance().track()));
	g_beepChannel = freeMainChannel();
	if (g_beepChannel)
		obs_set_output_source(g_beepChannel, g_beep);

	const int n = kSampleRate * kBeepMs / 1000;
	const int fade = kSampleRate * kFadeOutMs / 1000;
	std::vector<float> samples((size_t)n);
	for (int i = 0; i < n; i++) {
		const float env = i > n - fade ? (float)(n - i) / (float)fade : 1.0f;
		samples[(size_t)i] = kBeepAmplitude * env * (float)std::sin(kTwoPi * kBeepHz * (double)i / kSampleRate);
	}
	struct obs_source_audio audio = {};
	audio.data[0] = reinterpret_cast<const uint8_t *>(samples.data());
	audio.data[1] = reinterpret_cast<const uint8_t *>(samples.data());
	audio.frames = (uint32_t)n;
	audio.speakers = SPEAKERS_STEREO;
	audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audio.samples_per_sec = kSampleRate;
	audio.timestamp = t0;
	obs_source_output_audio(g_beep, &audio);

	writeEventFile(t0);
	obs_log(LOG_INFO, "sync test at %llu (in %d ms): offset %d ms, audio delay %.1f ms, video delay %d frames",
		(unsigned long long)t0, (int)((t0 - os_gettime_ns()) / 1000000), avsync::effectiveOffsetMs(),
		avsync::audioDelayMs(), avsync::videoDelayFrames());

	if (!g_cleanup) {
		g_cleanup = new QTimer();
		g_cleanup->setSingleShot(true);
		QObject::connect(g_cleanup, &QTimer::timeout, [] { finish(); });
	}
	g_cleanup->start(kLeadMs + std::max(kFlashMs, kBeepMs) + 1000);
	return t0;
}

uint64_t lastEventNs()
{
	return g_eventNs.load();
}

uint64_t flashFramesDrawn()
{
	return g_flashFrames.load();
}

void setRemoteTrigger(bool on)
{
	g_remoteUsers = std::max(0, g_remoteUsers + (on ? 1 : -1));
	on = g_remoteUsers > 0;
	(void)on;
#ifdef __APPLE__
	if (on && g_notifyToken == NOTIFY_TOKEN_INVALID) {
		notify_register_dispatch("company.theonair.vertical.sync-test", &g_notifyToken,
					 dispatch_get_main_queue(), ^(int) {
						 trigger();
					 });
	} else if (!on && g_notifyToken != NOTIFY_TOKEN_INVALID) {
		notify_cancel(g_notifyToken);
		g_notifyToken = NOTIFY_TOKEN_INVALID;
	}
#endif
}

void shutdown()
{
	g_remoteUsers = 1;
	setRemoteTrigger(false);
	delete g_cleanup;
	g_cleanup = nullptr;
	finish();
	obs_source_release(g_flash);
	obs_source_release(g_beep);
	g_flash = g_beep = nullptr;
}

} // namespace vertical::synctest
