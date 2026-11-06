#pragma once

/* Encoder planning: which outputs share which encoder, and which encoder id to
 * use. Pure C++17 (no OBS). The OBS layer feeds it the encoder types found at
 * runtime with obs_enum_encoder_types(). */

#include "destination.hpp"

#include <string>
#include <vector>

namespace vertical::core {

/* Something that needs encoders: a destination, the recording, the replay buffer. */
struct EncoderUser {
	std::string id;
	EncoderSettings settings;
};

struct VideoGroup {
	std::string key; /* EncoderSettings::videoKey() */
	VideoEncoding video;
	std::vector<std::string> users;
};

struct AudioGroup {
	std::string key; /* EncoderSettings::audioKey() */
	AudioEncoding audio;
	std::vector<std::string> users;
};

/* Users with identical video settings share one video encoder; users with
 * identical audio settings share one audio encoder. Groups keep the order of
 * first appearance, users keep their input order. */
struct EncoderPlan {
	std::vector<VideoGroup> video;
	std::vector<AudioGroup> audio;

	int videoGroupOf(const std::string &userId) const; /* -1 when absent */
	int audioGroupOf(const std::string &userId) const;
};

EncoderPlan planEncoders(const std::vector<EncoderUser> &users);

/* Destinations with identical full settings (video and audio): the grouping
 * shown to the user ("3 destinations, 2 encoders"). Disabled ones are skipped. */
struct DestinationGroup {
	std::string key;
	EncoderSettings settings;
	std::vector<std::string> destinationIds;
};
std::vector<DestinationGroup> groupDestinations(const std::vector<Destination> &destinations);

/* ---- encoder choice ---- */

struct EncoderCandidate {
	std::string id;
	std::string codec;       /* "h264", "hevc", "av1", "prores", ... */
	std::string displayName; /* obs_encoder_get_display_name() */
};

bool isHardwareEncoder(const EncoderCandidate &c);

struct EncoderChoice {
	std::string id;    /* empty when nothing usable was found */
	std::string codec; /* codec of the chosen encoder */
	bool hardware = false;
	bool requestedMissing = false; /* an explicit encoder id was asked but is not available */
	bool codecChanged = false;     /* hevc asked, h264 hardware chosen instead of software hevc */
};

/* "auto": hardware encoder of the wanted codec (VideoToolbox first on macOS),
 * then hardware H.264, then x264 as the last resort. */
EncoderChoice chooseVideoEncoder(const std::vector<EncoderCandidate> &available, const VideoEncoding &wanted);

/* CoreAudio AAC on macOS, then FFmpeg AAC, then FDK AAC. Empty when none. */
std::string chooseAudioEncoder(const std::vector<std::string> &availableAudioIds);

} // namespace vertical::core
