#pragma once

/* The vertical audio mix. OBS has one audio output with six mixer tracks; every
 * source has one bit per track. Vertical manages one track (the "vertical track"):
 * a source is on it when it is on the main mix track (track 1) and the user did not
 * exclude it in the Vertical settings. Mute and volume stay global (the main mixer
 * keeps control). Pure C++17. */

#include "json.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace vertical::core {

constexpr int kTrackCount = 6;
constexpr int kMainTrack = 1;
/* Used when every track is taken by OBS's own outputs, or nothing is known. */
constexpr int kFallbackVerticalTrack = 6;

inline uint32_t trackBit(int track)
{
	return (track >= 1 && track <= kTrackCount) ? (1u << (track - 1)) : 0u;
}

/* What OBS's own outputs (Settings > Output of the current profile) use. */
struct ObsOutputConfig {
	bool advanced = false; /* Output mode: Simple or Advanced */

	/* Simple mode: the stream uses track 1. The recording uses track 1 too when its
	 * quality is "Same as stream", else the tracks of RecTracks. */
	bool simpleRecSameAsStream = true;
	uint32_t simpleRecTracks = 1;
	bool simpleVodTrack = false; /* "Twitch VOD track" (uses track 2) */

	/* Advanced mode. */
	int advStreamTrack = 1;       /* AdvOut/TrackIndex */
	uint32_t advRecTracks = 1;    /* AdvOut/RecTracks (standard recording) */
	bool advRecFFmpeg = false;    /* AdvOut/RecType == FFmpeg */
	uint32_t advFFmpegTracks = 1; /* AdvOut/FFAudioMixes */
	bool advVodTrack = false;     /* AdvOut/VodTrackEnabled */
	int advVodTrackIndex = 2;     /* AdvOut/VodTrackIndex */
};

/* Bit mask (bit 0 = track 1) of the tracks OBS's own stream, recording and VOD track use. */
uint32_t usedTracks(const ObsOutputConfig &config);

/* The vertical track: `current` when it is still free, else the first free track
 * counting down from track 6 (users fill tracks from 1 up), else track 6. */
int chooseVerticalTrack(uint32_t used, int current);

/* The mixers bit mask of a source with its vertical-track bit set from the rule
 * "on the main track AND not excluded". Other bits are never changed. */
uint32_t verticalMixers(uint32_t mixers, int verticalTrack, bool excluded, int mainTrack = kMainTrack);

/* Sources the user took out of the vertical mix, by source uuid (stable across
 * renames and saves). New sources are included by default. */
class AudioExclusions {
public:
	bool excluded(const std::string &uuid) const { return uuids_.count(uuid) != 0; }
	/* Returns true when the set changed. */
	bool setExcluded(const std::string &uuid, bool excluded);
	/* Drops the uuids not in `existing` (sources deleted for good). Returns the count dropped. */
	size_t prune(const std::vector<std::string> &existing);
	size_t size() const { return uuids_.size(); }
	const std::set<std::string> &uuids() const { return uuids_; }

	Json toJson() const;
	static AudioExclusions fromJson(const Json &j);

	bool operator==(const AudioExclusions &o) const { return uuids_ == o.uuids_; }
	bool operator!=(const AudioExclusions &o) const { return !(*this == o); }

private:
	std::set<std::string> uuids_;
};

/* One audio input of the main (16:9) scenes or a global audio device, as listed in
 * the Vertical settings. */
struct AudioSourceInfo {
	std::string uuid;
	std::string name;
	uint32_t mixers = 0;
	bool global = false; /* Desktop Audio, Mic/Aux: always mixed, not in a scene */
};

struct AudioRow {
	std::string uuid;
	std::string name;
	bool sent = true;      /* checkbox "Sent to vertical" */
	bool onMainMix = true; /* on track 1: otherwise it is not in the main mix either */
	bool global = false;
};

/* Rows for the settings list: one per uuid, global devices first, then by name
 * (case-insensitive). */
std::vector<AudioRow> audioRows(const std::vector<AudioSourceInfo> &sources, const AudioExclusions &exclusions,
				int mainTrack = kMainTrack);

} // namespace vertical::core
