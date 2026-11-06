#pragma once

/* Streaming destinations of the vertical output: model, URL parsing and
 * validation. Pure C++17 (no OBS): unit-tested in tests/test-destination.cpp. */

#include <optional>
#include <string>
#include <vector>

namespace vertical::core {

enum class Platform { TikTok, YouTube, Instagram, Facebook, CustomRtmp, CustomSrt };
enum class Protocol { Unknown, Rtmp, Rtmps, Srt };

/* Stable ids used in the saved settings ("tiktok", "youtube", ...). */
const char *platformId(Platform p);
Platform platformFromId(const std::string &id, Platform fallback = Platform::CustomRtmp);
const std::vector<Platform> &allPlatforms();

struct PlatformInfo {
	const char *displayName;
	/* Server filled in when the user picks the platform; empty when the
	 * platform hands out a new server for every live (TikTok, Instagram). */
	const char *defaultServer;
	bool allowRtmp;
	bool allowRtmps;
	bool allowSrt;
	bool keyRequired;
	bool hevcAllowed;
	int recommendedKbps;
	int maxKbps; /* soft limit: above it the platform may refuse or re-encode */
};
const PlatformInfo &platformInfo(Platform p);

/* Platform a server URL points to, from its host: TikTok (tiktok*.com, e.g.
 * push-rtmp-l11-va01.tiktokcdn.com), YouTube (*.youtube.com), Instagram
 * (*.instagram.com, edgetee-upload-*.fbcdn.net), Facebook (*.facebook.com,
 * other *.fbcdn.net), else SRT for an srt:// URL. Empty for any other server. */
std::optional<Platform> platformFromServer(const std::string &server);

struct StreamUrl {
	bool valid = false;
	Protocol protocol = Protocol::Unknown;
	std::string host;
	int port = 0;      /* explicit port, 0 when absent */
	std::string path;  /* without the leading slash, may be empty */
	std::string query; /* without the '?', may be empty */
	std::string error; /* short reason when !valid */
};
StreamUrl parseStreamUrl(const std::string &url);
int defaultPort(Protocol p); /* 1935, 443, 0 (SRT has none) */
const char *protocolName(Protocol p);

struct VideoEncoding {
	std::string encoder = "auto"; /* "auto" or an OBS encoder id */
	std::string codec = "h264";   /* "h264" or "hevc" (used when encoder is "auto") */
	int bitrateKbps = 6000;
	int keyintSec = 2;
	std::string rateControl = "CBR";
	std::string profile = "high";
	bool bframes = true;

	bool operator==(const VideoEncoding &o) const;
	bool operator!=(const VideoEncoding &o) const { return !(*this == o); }
};

struct AudioEncoding {
	int bitrateKbps = 160;
	int track = 0; /* 0 = the vertical mix (the track Vertical manages), 1..6 = that OBS mixer track */

	bool operator==(const AudioEncoding &o) const { return bitrateKbps == o.bitrateKbps && track == o.track; }
	bool operator!=(const AudioEncoding &o) const { return !(*this == o); }
};

struct EncoderSettings {
	VideoEncoding video;
	AudioEncoding audio;

	bool operator==(const EncoderSettings &o) const { return video == o.video && audio == o.audio; }
	bool operator!=(const EncoderSettings &o) const { return !(*this == o); }
	/* Canonical text of the settings: two equal settings have the same key. */
	std::string key() const;
	std::string videoKey() const;
	std::string audioKey() const;
};

struct Destination {
	std::string id; /* stable, generated once */
	std::string name;
	Platform platform = Platform::CustomRtmp;
	std::string server;
	std::string streamKey; /* secret: never logged, see redactKey() */
	bool enabled = true;
	EncoderSettings encoding;
};

/* A blank destination prefilled for a platform. */
Destination makeDestination(Platform p, const std::string &id, const std::string &name);

enum class Severity { Error, Warning };

enum class IssueCode {
	NameEmpty,
	ServerEmpty,
	ServerInvalid,
	ProtocolNotAllowed, /* e.g. plain rtmp:// for Instagram */
	PortInvalid,
	SrtPortMissing,
	KeyMissing,
	KeyHasSpaces,
	BitrateInvalid,
	BitrateAboveLimit,
	KeyintInvalid,
	KeyintNotRecommended,
	CodecNotAllowed,
	AudioTrackInvalid,
	AudioBitrateInvalid,
};

struct Issue {
	IssueCode code;
	Severity severity;
};

/* Locale key of an issue, e.g. "Issue.ServerInvalid". */
const char *issueKey(IssueCode c);

std::vector<Issue> validate(const Destination &d);
bool hasErrors(const std::vector<Issue> &issues);

/* "live_123456789_abcdef" -> "live…cdef"; short keys become "••••". */
std::string redactKey(const std::string &key);

/* OBS output type that carries a protocol: rtmp_output or ffmpeg_mpegts_muxer. */
const char *outputTypeFor(Protocol p);

/* Generates a short unique id (hex) from a seed, avoiding the given ids. */
std::string makeId(unsigned long long seed, const std::vector<std::string> &taken);

} // namespace vertical::core
