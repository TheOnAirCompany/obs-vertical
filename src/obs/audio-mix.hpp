#pragma once

/* The vertical audio mix: one OBS mixer track managed by Vertical (see
 * core/audio-mix.hpp). Keeps the vertical-track bit of every audio source equal to
 * "on track 1 AND not excluded", live (sources created, loaded, removed, mixers changed,
 * collection switch), lists the audio inputs of the main scenes for the settings, and
 * measures the level of the mix for the dock meter. UI thread unless noted. */

#include "core/audio-mix.hpp"

#include <obs.hpp>

#include <QObject>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vertical {

class AudioMix : public QObject {
	Q_OBJECT

public:
	static AudioMix &instance();

	void start();    /* obs_module_load */
	void shutdown(); /* OBS_FRONTEND_EVENT_EXIT */

	/* Collection loaded or profile changed: choose the track, apply every source. */
	void refresh();

	/* The managed track, 1..6. Any thread. */
	int track() const { return track_.load(); }
	/* OBS mix index (track - 1) for audio encoders and raw callbacks. Any thread. */
	size_t mixIndex() const { return (size_t)(track() - 1); }
	/* What OBS's own outputs use (bit 0 = track 1). */
	uint32_t obsUsedTracks() const;

	/* Audio inputs of the main (16:9) scenes and the global audio devices. */
	std::vector<core::AudioSourceInfo> mainSources() const;
	std::vector<core::AudioRow> rows() const;
	/* Replaces the exclusions (settings dialog) and applies them. */
	void setExclusions(const core::AudioExclusions &exclusions);
	void setExcluded(const std::string &uuid, bool excluded);
	bool excluded(const std::string &uuid) const;

	/* Recomputes the vertical bit of one source (any thread). */
	void applySource(obs_source_t *source);
	void applyAll();

	/* ---- level meter (stereo peaks of the vertical track) ---- */
	void acquireMeter();
	void releaseMeter();
	bool meterActive() const { return meterUsers_ > 0; }
	/* Peaks (linear) received since the last call; false when no audio arrived. */
	bool takePeaks(float &left, float &right);
	/* Number of chunks received by the meter since it started (self-test). */
	uint64_t meterChunks() const { return meterChunks_.load(); }

	/* Private sources of Vertical (sync test) are never managed. */
	static bool isOwnSource(obs_source_t *source);

signals:
	void sourcesChanged();
	void trackChanged(int track);

private:
	AudioMix() = default;
	void syncSnapshot();
	void connectSource(obs_source_t *source);
	void attachMeter();
	void detachMeter();
	void queueSourcesChanged();

	static void sourceCreateCb(void *data, calldata_t *cd);
	static void sourceLoadCb(void *data, calldata_t *cd);
	static void sourceRemoveCb(void *data, calldata_t *cd);
	static void sourceRenameCb(void *data, calldata_t *cd);
	static void mixersCb(void *data, calldata_t *cd);
	static void meterCb(void *param, size_t mix, struct audio_data *data);

	std::atomic<int> track_{core::kFallbackVerticalTrack};
	mutable std::mutex lock_; /* guards excluded_ (read from other threads) */
	std::set<std::string> excluded_;
	bool started_ = false;
	std::atomic<bool> changeQueued_{false};

	int meterUsers_ = 0;
	size_t meterMix_ = 0;
	bool meterAttached_ = false;
	std::atomic<uint32_t> peakL_{0}; /* float bits */
	std::atomic<uint32_t> peakR_{0};
	std::atomic<bool> peakFresh_{false};
	std::atomic<uint64_t> meterChunks_{0};
};

} // namespace vertical
