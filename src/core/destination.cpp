#include "destination.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace vertical::core {

namespace {

const PlatformInfo kTikTok{"TikTok", "", true, true, false, true, false, 6000, 8000};
const PlatformInfo kYouTube{"YouTube", "rtmps://a.rtmps.youtube.com:443/live2", true, true, false, true, true, 9000,
			    51000};
const PlatformInfo kInstagram{"Instagram", "", false, true, false, true, false, 4000, 6000};
const PlatformInfo kFacebook{
	"Facebook", "rtmps://live-api-s.facebook.com:443/rtmp/", false, true, false, true, false, 6000, 9000};
const PlatformInfo kCustomRtmp{"RTMP", "", true, true, false, false, true, 6000, 100000};
const PlatformInfo kCustomSrt{"SRT", "", false, false, true, false, true, 6000, 100000};

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
		a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
		b--;
	return s.substr(a, b - a);
}

bool validHostChar(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_';
}

} // namespace

const char *platformId(Platform p)
{
	switch (p) {
	case Platform::TikTok:
		return "tiktok";
	case Platform::YouTube:
		return "youtube";
	case Platform::Instagram:
		return "instagram";
	case Platform::Facebook:
		return "facebook";
	case Platform::CustomRtmp:
		return "rtmp";
	case Platform::CustomSrt:
		return "srt";
	}
	return "rtmp";
}

Platform platformFromId(const std::string &id, Platform fallback)
{
	for (Platform p : allPlatforms())
		if (id == platformId(p))
			return p;
	return fallback;
}

const std::vector<Platform> &allPlatforms()
{
	static const std::vector<Platform> all{Platform::TikTok,   Platform::YouTube,    Platform::Instagram,
					       Platform::Facebook, Platform::CustomRtmp, Platform::CustomSrt};
	return all;
}

const PlatformInfo &platformInfo(Platform p)
{
	switch (p) {
	case Platform::TikTok:
		return kTikTok;
	case Platform::YouTube:
		return kYouTube;
	case Platform::Instagram:
		return kInstagram;
	case Platform::Facebook:
		return kFacebook;
	case Platform::CustomRtmp:
		return kCustomRtmp;
	case Platform::CustomSrt:
		return kCustomSrt;
	}
	return kCustomRtmp;
}

int defaultPort(Protocol p)
{
	switch (p) {
	case Protocol::Rtmp:
		return 1935;
	case Protocol::Rtmps:
		return 443;
	default:
		return 0;
	}
}

const char *protocolName(Protocol p)
{
	switch (p) {
	case Protocol::Rtmp:
		return "RTMP";
	case Protocol::Rtmps:
		return "RTMPS";
	case Protocol::Srt:
		return "SRT";
	default:
		return "?";
	}
}

StreamUrl parseStreamUrl(const std::string &input)
{
	StreamUrl u;
	const std::string url = trim(input);
	if (url.empty()) {
		u.error = "empty";
		return u;
	}
	const size_t sep = url.find("://");
	if (sep == std::string::npos) {
		u.error = "missing scheme";
		return u;
	}
	const std::string scheme = lower(url.substr(0, sep));
	if (scheme == "rtmp")
		u.protocol = Protocol::Rtmp;
	else if (scheme == "rtmps")
		u.protocol = Protocol::Rtmps;
	else if (scheme == "srt")
		u.protocol = Protocol::Srt;
	else {
		u.error = "unsupported scheme";
		return u;
	}

	std::string rest = url.substr(sep + 3);
	for (char c : rest)
		if (std::isspace(static_cast<unsigned char>(c))) {
			u.error = "spaces in URL";
			return u;
		}

	const size_t q = rest.find('?');
	if (q != std::string::npos) {
		u.query = rest.substr(q + 1);
		rest = rest.substr(0, q);
	}
	std::string authority = rest;
	const size_t slash = rest.find('/');
	if (slash != std::string::npos) {
		authority = rest.substr(0, slash);
		u.path = rest.substr(slash + 1);
	}
	if (authority.find('@') != std::string::npos) {
		u.error = "credentials in URL";
		return u;
	}

	std::string host = authority;
	std::string port;
	if (!authority.empty() && authority[0] == '[') {
		const size_t close = authority.find(']');
		if (close == std::string::npos) {
			u.error = "bad IPv6 host";
			return u;
		}
		host = authority.substr(1, close - 1);
		const std::string after = authority.substr(close + 1);
		if (!after.empty()) {
			if (after[0] != ':') {
				u.error = "bad host";
				return u;
			}
			port = after.substr(1);
			if (port.empty()) {
				u.error = "bad port";
				return u;
			}
		}
		for (char c : host)
			if (!(std::isxdigit(static_cast<unsigned char>(c)) || c == ':' || c == '.')) {
				u.error = "bad IPv6 host";
				return u;
			}
	} else {
		const size_t colon = authority.rfind(':');
		if (colon != std::string::npos) {
			host = authority.substr(0, colon);
			port = authority.substr(colon + 1);
			if (port.empty()) {
				u.error = "bad port";
				return u;
			}
		}
		for (char c : host)
			if (!validHostChar(c)) {
				u.error = "bad host";
				return u;
			}
		if (!host.empty() && (host.front() == '.' || host.back() == '.' || host.front() == '-')) {
			u.error = "bad host";
			return u;
		}
	}
	if (host.empty()) {
		u.error = "missing host";
		return u;
	}
	if (!port.empty()) {
		if (port.size() > 5 ||
		    !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
			u.error = "bad port";
			return u;
		}
		const int p = std::stoi(port);
		if (p < 1 || p > 65535) {
			u.error = "bad port";
			return u;
		}
		u.port = p;
	}
	u.host = host;
	u.valid = true;
	return u;
}

bool VideoEncoding::operator==(const VideoEncoding &o) const
{
	return encoder == o.encoder && codec == o.codec && bitrateKbps == o.bitrateKbps && keyintSec == o.keyintSec &&
	       rateControl == o.rateControl && profile == o.profile && bframes == o.bframes;
}

std::string EncoderSettings::videoKey() const
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "|%d|%d|", video.bitrateKbps, video.keyintSec);
	return "v:" + video.encoder + "|" + video.codec + buf + video.rateControl + "|" + video.profile + "|" +
	       (video.bframes ? "b" : "nob");
}

std::string EncoderSettings::audioKey() const
{
	char buf[48];
	std::snprintf(buf, sizeof(buf), "a:%d|t%d", audio.bitrateKbps, audio.track);
	return buf;
}

std::string EncoderSettings::key() const
{
	return videoKey() + ";" + audioKey();
}

Destination makeDestination(Platform p, const std::string &id, const std::string &name)
{
	Destination d;
	d.id = id;
	d.name = name;
	d.platform = p;
	const PlatformInfo &info = platformInfo(p);
	d.server = info.defaultServer;
	d.encoding.video.bitrateKbps = info.recommendedKbps;
	return d;
}

const char *issueKey(IssueCode c)
{
	switch (c) {
	case IssueCode::NameEmpty:
		return "Issue.NameEmpty";
	case IssueCode::ServerEmpty:
		return "Issue.ServerEmpty";
	case IssueCode::ServerInvalid:
		return "Issue.ServerInvalid";
	case IssueCode::ProtocolNotAllowed:
		return "Issue.ProtocolNotAllowed";
	case IssueCode::PortInvalid:
		return "Issue.PortInvalid";
	case IssueCode::SrtPortMissing:
		return "Issue.SrtPortMissing";
	case IssueCode::KeyMissing:
		return "Issue.KeyMissing";
	case IssueCode::KeyHasSpaces:
		return "Issue.KeyHasSpaces";
	case IssueCode::BitrateInvalid:
		return "Issue.BitrateInvalid";
	case IssueCode::BitrateAboveLimit:
		return "Issue.BitrateAboveLimit";
	case IssueCode::KeyintInvalid:
		return "Issue.KeyintInvalid";
	case IssueCode::KeyintNotRecommended:
		return "Issue.KeyintNotRecommended";
	case IssueCode::CodecNotAllowed:
		return "Issue.CodecNotAllowed";
	case IssueCode::AudioTrackInvalid:
		return "Issue.AudioTrackInvalid";
	case IssueCode::AudioBitrateInvalid:
		return "Issue.AudioBitrateInvalid";
	}
	return "Issue.Unknown";
}

std::vector<Issue> validate(const Destination &d)
{
	std::vector<Issue> out;
	const auto add = [&](IssueCode c, Severity s) {
		out.push_back({c, s});
	};
	const PlatformInfo &info = platformInfo(d.platform);

	if (trim(d.name).empty())
		add(IssueCode::NameEmpty, Severity::Error);

	if (trim(d.server).empty()) {
		add(IssueCode::ServerEmpty, Severity::Error);
	} else {
		const StreamUrl u = parseStreamUrl(d.server);
		if (!u.valid) {
			if (u.error == "bad port")
				add(IssueCode::PortInvalid, Severity::Error);
			else
				add(IssueCode::ServerInvalid, Severity::Error);
		} else {
			const bool allowed = (u.protocol == Protocol::Rtmp && info.allowRtmp) ||
					     (u.protocol == Protocol::Rtmps && info.allowRtmps) ||
					     (u.protocol == Protocol::Srt && info.allowSrt);
			if (!allowed)
				add(IssueCode::ProtocolNotAllowed, Severity::Error);
			if (u.protocol == Protocol::Srt && u.port == 0)
				add(IssueCode::SrtPortMissing, Severity::Error);
		}
	}

	const std::string key = d.streamKey;
	if (info.keyRequired && trim(key).empty())
		add(IssueCode::KeyMissing, Severity::Error);
	if (key.find_first_of(" \t\r\n") != std::string::npos)
		add(IssueCode::KeyHasSpaces, Severity::Warning);

	const VideoEncoding &v = d.encoding.video;
	if (v.bitrateKbps < 200 || v.bitrateKbps > 100000)
		add(IssueCode::BitrateInvalid, Severity::Error);
	else if (v.bitrateKbps > info.maxKbps)
		add(IssueCode::BitrateAboveLimit, Severity::Warning);

	if (v.keyintSec < 0 || v.keyintSec > 20)
		add(IssueCode::KeyintInvalid, Severity::Error);
	else if (v.keyintSec != 2 && d.platform != Platform::CustomRtmp && d.platform != Platform::CustomSrt)
		add(IssueCode::KeyintNotRecommended, Severity::Warning);

	if (v.codec != "h264" && v.codec != "hevc")
		add(IssueCode::CodecNotAllowed, Severity::Error);
	else if (v.codec == "hevc" && !info.hevcAllowed)
		add(IssueCode::CodecNotAllowed, Severity::Error);

	const AudioEncoding &a = d.encoding.audio;
	if (a.track < 1 || a.track > 6)
		add(IssueCode::AudioTrackInvalid, Severity::Error);
	if (a.bitrateKbps < 32 || a.bitrateKbps > 512)
		add(IssueCode::AudioBitrateInvalid, Severity::Error);

	return out;
}

bool hasErrors(const std::vector<Issue> &issues)
{
	return std::any_of(issues.begin(), issues.end(), [](const Issue &i) { return i.severity == Severity::Error; });
}

std::string redactKey(const std::string &key)
{
	if (key.size() < 12)
		return key.empty() ? std::string() : std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
	return key.substr(0, 4) + "\xE2\x80\xA6" + key.substr(key.size() - 4);
}

const char *outputTypeFor(Protocol p)
{
	return p == Protocol::Srt ? "ffmpeg_mpegts_muxer" : "rtmp_output";
}

std::string makeId(unsigned long long seed, const std::vector<std::string> &taken)
{
	/* splitmix64: well spread ids from a time or counter seed. */
	for (int attempt = 0; attempt < 1000; attempt++) {
		unsigned long long z = seed + 0x9E3779B97F4A7C15ull * (unsigned long long)(attempt + 1);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		z ^= z >> 31;
		char buf[24];
		std::snprintf(buf, sizeof(buf), "%012llx", z & 0xFFFFFFFFFFFFull);
		const std::string id(buf);
		if (std::find(taken.begin(), taken.end(), id) == taken.end())
			return id;
	}
	return "x" + std::to_string(taken.size());
}

} // namespace vertical::core
