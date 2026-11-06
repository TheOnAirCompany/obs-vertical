#include "obs/audio-mix.hpp"

#include "core/meter.hpp"
#include "obs/vertical.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/config-file.h>

#include <QMetaObject>

#include <algorithm>
#include <cstring>

namespace vertical {

namespace {

constexpr uint32_t kGlobalAudioChannels = 6; /* main view channels 1..6: Desktop Audio, Mic/Aux */

bool hasAudio(obs_source_t *s)
{
	return s && (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) &&
	       obs_source_get_type(s) == OBS_SOURCE_TYPE_INPUT;
}

uint32_t floatBits(float f)
{
	uint32_t u;
	std::memcpy(&u, &f, sizeof(u));
	return u;
}

float bitsFloat(uint32_t u)
{
	float f;
	std::memcpy(&f, &u, sizeof(f));
	return f;
}

/* Non-negative floats order like their bit patterns. */
void storeMax(std::atomic<uint32_t> &a, float v)
{
	const uint32_t bits = floatBits(v);
	uint32_t cur = a.load(std::memory_order_relaxed);
	while (bits > cur && !a.compare_exchange_weak(cur, bits, std::memory_order_relaxed)) {
	}
}

core::ObsOutputConfig readOutputConfig()
{
	core::ObsOutputConfig c;
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return c;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	c.advanced = mode && std::strcmp(mode, "Advanced") == 0;
	const char *quality = config_get_string(cfg, "SimpleOutput", "RecQuality");
	c.simpleRecSameAsStream = !quality || !*quality || std::strcmp(quality, "Stream") == 0;
	c.simpleRecTracks = (uint32_t)config_get_int(cfg, "SimpleOutput", "RecTracks");
	c.simpleVodTrack = config_get_bool(cfg, "SimpleOutput", "VodTrackEnabled");
	c.advStreamTrack = (int)config_get_int(cfg, "AdvOut", "TrackIndex");
	const char *recType = config_get_string(cfg, "AdvOut", "RecType");
	c.advRecFFmpeg = recType && std::strcmp(recType, "FFmpeg") == 0;
	c.advRecTracks = (uint32_t)config_get_int(cfg, "AdvOut", "RecTracks");
	c.advFFmpegTracks = (uint32_t)config_get_int(cfg, "AdvOut", "FFAudioMixes");
	c.advVodTrack = config_get_bool(cfg, "AdvOut", "VodTrackEnabled");
	c.advVodTrackIndex = (int)config_get_int(cfg, "AdvOut", "VodTrackIndex");
	if (c.advStreamTrack < 1)
		c.advStreamTrack = 1;
	return c;
}

struct Collector {
	std::vector<core::AudioSourceInfo> *out;
	std::set<obs_source_t *> visited;
};

void collectScene(obs_scene_t *scene, Collector &c);

bool collectItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto &c = *static_cast<Collector *>(param);
	obs_source_t *s = obs_sceneitem_get_source(item);
	if (!s || !c.visited.insert(s).second)
		return true;
	if (obs_sceneitem_is_group(item)) {
		collectScene(obs_sceneitem_group_get_scene(item), c);
	} else if (obs_source_is_scene(s)) {
		collectScene(obs_scene_from_source(s), c);
	} else if (hasAudio(s) && !AudioMix::isOwnSource(s)) {
		c.out->push_back(
			{obs_source_get_uuid(s), obs_source_get_name(s), obs_source_get_audio_mixers(s), false});
	}
	return true;
}

void collectScene(obs_scene_t *scene, Collector &c)
{
	if (scene)
		obs_scene_enum_items(scene, collectItem, &c);
}

} // namespace

AudioMix &AudioMix::instance()
{
	static AudioMix *m = new AudioMix();
	return *m;
}

bool AudioMix::isOwnSource(obs_source_t *source)
{
	const char *id = source ? obs_source_get_id(source) : nullptr;
	return id && std::strncmp(id, "vertical_", 9) == 0;
}

/* =================================================================== lifecycle */

void AudioMix::start()
{
	if (started_)
		return;
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_create", sourceCreateCb, this);
	signal_handler_connect(sh, "source_load", sourceLoadCb, this);
	signal_handler_connect(sh, "source_remove", sourceRemoveCb, this);
	signal_handler_connect(sh, "source_rename", sourceRenameCb, this);
	started_ = true;
}

void AudioMix::shutdown()
{
	if (!started_)
		return;
	detachMeter();
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_disconnect(sh, "source_create", sourceCreateCb, this);
	signal_handler_disconnect(sh, "source_load", sourceLoadCb, this);
	signal_handler_disconnect(sh, "source_remove", sourceRemoveCb, this);
	signal_handler_disconnect(sh, "source_rename", sourceRenameCb, this);
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			signal_handler_disconnect(obs_source_get_signal_handler(s), "audio_mixers", mixersCb, param);
			return true;
		},
		this);
	started_ = false;
}

uint32_t AudioMix::obsUsedTracks() const
{
	return core::usedTracks(readOutputConfig());
}

void AudioMix::refresh()
{
	core::Settings &s = Vertical::instance().settings();
	const uint32_t used = obsUsedTracks();
	const int previous = track_.load();
	const int chosen = core::chooseVerticalTrack(used, s.audio.track);
	if (s.audio.track && s.audio.track != chosen)
		obs_log(LOG_WARNING, "vertical audio: track %d is now used by OBS, moving the vertical mix to track %d",
			s.audio.track, chosen);
	s.audio.track = chosen;
	track_ = chosen;

	/* Exclusions of sources that no longer exist (deleted for good). */
	std::vector<std::string> existing;
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			static_cast<std::vector<std::string> *>(param)->push_back(obs_source_get_uuid(src));
			return true;
		},
		&existing);
	if (const size_t dropped = s.audio.exclusions.prune(existing))
		obs_log(LOG_INFO, "vertical audio: %zu exclusions of deleted sources dropped", dropped);
	syncSnapshot();
	applyAll();
	obs_log(LOG_INFO, "vertical audio: mix on track %d (OBS outputs use 0x%02x), %zu sources excluded", chosen,
		used, s.audio.exclusions.size());
	if (previous != chosen) {
		if (meterAttached_) {
			detachMeter();
			attachMeter();
		}
		emit trackChanged(chosen);
	}
	emit sourcesChanged();
}

void AudioMix::syncSnapshot()
{
	std::lock_guard<std::mutex> guard(lock_);
	excluded_ = Vertical::instance().settings().audio.exclusions.uuids();
}

/* =================================================================== bits */

bool AudioMix::excluded(const std::string &uuid) const
{
	std::lock_guard<std::mutex> guard(lock_);
	return excluded_.count(uuid) != 0;
}

void AudioMix::applySource(obs_source_t *source)
{
	if (!hasAudio(source) || isOwnSource(source))
		return;
	const uint32_t mixers = obs_source_get_audio_mixers(source);
	const uint32_t want = core::verticalMixers(mixers, track(), excluded(obs_source_get_uuid(source)));
	if (want != mixers)
		obs_source_set_audio_mixers(source, want);
}

void AudioMix::applyAll()
{
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			auto *self = static_cast<AudioMix *>(param);
			self->connectSource(s);
			self->applySource(s);
			return true;
		},
		this);
}

void AudioMix::connectSource(obs_source_t *source)
{
	if (!hasAudio(source) || isOwnSource(source))
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	/* Connecting twice would call us twice: disconnect first. */
	signal_handler_disconnect(sh, "audio_mixers", mixersCb, this);
	signal_handler_connect(sh, "audio_mixers", mixersCb, this);
}

void AudioMix::setExclusions(const core::AudioExclusions &exclusions)
{
	Vertical::instance().settings().audio.exclusions = exclusions;
	syncSnapshot();
	applyAll();
	emit sourcesChanged();
}

void AudioMix::setExcluded(const std::string &uuid, bool excluded)
{
	if (!Vertical::instance().settings().audio.exclusions.setExcluded(uuid, excluded))
		return;
	syncSnapshot();
	OBSSourceAutoRelease s = obs_get_source_by_uuid(uuid.c_str());
	applySource(s);
	emit sourcesChanged();
}

std::vector<core::AudioSourceInfo> AudioMix::mainSources() const
{
	std::vector<core::AudioSourceInfo> out;
	Collector c{&out, {}};
	for (uint32_t ch = 1; ch <= kGlobalAudioChannels; ch++) {
		OBSSourceAutoRelease s = obs_get_output_source(ch);
		if (hasAudio(s) && c.visited.insert(s.Get()).second)
			out.push_back(
				{obs_source_get_uuid(s), obs_source_get_name(s), obs_source_get_audio_mixers(s), true});
	}
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++)
		collectScene(obs_scene_from_source(scenes.sources.array[i]), c);
	obs_frontend_source_list_free(&scenes);
	return out;
}

std::vector<core::AudioRow> AudioMix::rows() const
{
	return core::audioRows(mainSources(), Vertical::instance().settings().audio.exclusions);
}

void AudioMix::queueSourcesChanged()
{
	if (changeQueued_.exchange(true))
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			changeQueued_ = false;
			emit sourcesChanged();
		},
		Qt::QueuedConnection);
}

/* =================================================================== signals */

void AudioMix::sourceCreateCb(void *data, calldata_t *cd)
{
	auto *self = static_cast<AudioMix *>(data);
	auto *s = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	self->connectSource(s);
	self->applySource(s); /* new sources are included by default */
	if (hasAudio(s))
		self->queueSourcesChanged();
}

void AudioMix::sourceLoadCb(void *data, calldata_t *cd)
{
	/* Loaded from a collection: its saved mixers are set, apply our rule on top. */
	auto *self = static_cast<AudioMix *>(data);
	auto *s = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	self->connectSource(s);
	self->applySource(s);
}

void AudioMix::sourceRemoveCb(void *data, calldata_t *cd)
{
	auto *s = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (hasAudio(s) || obs_source_is_scene(s))
		static_cast<AudioMix *>(data)->queueSourcesChanged();
}

void AudioMix::sourceRenameCb(void *data, calldata_t *cd)
{
	auto *s = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (hasAudio(s))
		static_cast<AudioMix *>(data)->queueSourcesChanged();
}

void AudioMix::mixersCb(void *data, calldata_t *cd)
{
	/* The user changed the tracks of a source (Advanced Audio Properties): the
	 * vertical bit follows track 1. Our own correction comes back here once and
	 * stops, the bit being right. */
	auto *self = static_cast<AudioMix *>(data);
	auto *s = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	const uint32_t mixers = (uint32_t)calldata_int(cd, "mixers");
	if (!s)
		return;
	const uint32_t want = core::verticalMixers(mixers, self->track(), self->excluded(obs_source_get_uuid(s)));
	if (want != mixers)
		calldata_set_int(cd, "mixers", want); /* the signal is sent before the value is stored */
	self->queueSourcesChanged();
}

/* =================================================================== meter */

void AudioMix::acquireMeter()
{
	if (meterUsers_++ == 0)
		attachMeter();
}

void AudioMix::releaseMeter()
{
	if (meterUsers_ > 0 && --meterUsers_ == 0)
		detachMeter();
}

void AudioMix::attachMeter()
{
	if (meterAttached_)
		return;
	struct audio_convert_info conv = {};
	conv.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
	conv.format = AUDIO_FORMAT_FLOAT_PLANAR;
	conv.speakers = SPEAKERS_STEREO;
	meterMix_ = mixIndex();
	obs_add_raw_audio_callback(meterMix_, &conv, meterCb, this);
	meterAttached_ = true;
}

void AudioMix::detachMeter()
{
	if (!meterAttached_)
		return;
	obs_remove_raw_audio_callback(meterMix_, meterCb, this);
	meterAttached_ = false;
	peakFresh_ = false;
}

void AudioMix::meterCb(void *param, size_t, struct audio_data *data)
{
	auto *self = static_cast<AudioMix *>(param);
	if (!data || !data->frames)
		return;
	const float l = core::peakOf(reinterpret_cast<const float *>(data->data[0]), data->frames);
	const float r = data->data[1] ? core::peakOf(reinterpret_cast<const float *>(data->data[1]), data->frames) : l;
	storeMax(self->peakL_, l);
	storeMax(self->peakR_, r);
	self->peakFresh_.store(true, std::memory_order_release);
	self->meterChunks_.fetch_add(1, std::memory_order_relaxed);
}

bool AudioMix::takePeaks(float &left, float &right)
{
	if (!peakFresh_.exchange(false, std::memory_order_acq_rel)) {
		left = right = 0.0f;
		return false;
	}
	left = bitsFloat(peakL_.exchange(0));
	right = bitsFloat(peakR_.exchange(0));
	return true;
}

} // namespace vertical
