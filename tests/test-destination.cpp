#include "test.hpp"

#include "destination.hpp"

#include <algorithm>

using namespace vertical::core;

namespace {

bool hasIssue(const std::vector<Issue> &v, IssueCode c)
{
	return std::any_of(v.begin(), v.end(), [&](const Issue &i) { return i.code == c; });
}

Destination valid(Platform p, const std::string &server)
{
	Destination d = makeDestination(p, "id1", "Main");
	d.server = server;
	d.streamKey = "live_abcdefghijklmnop";
	return d;
}

} // namespace

TEST("url: rtmp, rtmps and srt with ports, paths and queries")
{
	StreamUrl u = parseStreamUrl("rtmp://a.rtmp.youtube.com/live2");
	CHECK(u.valid);
	CHECK(u.protocol == Protocol::Rtmp);
	CHECK_EQ(u.host, std::string("a.rtmp.youtube.com"));
	CHECK_EQ(u.port, 0);
	CHECK_EQ(u.path, std::string("live2"));

	u = parseStreamUrl("  RTMPS://live-api-s.facebook.com:443/rtmp/  ");
	CHECK(u.valid);
	CHECK(u.protocol == Protocol::Rtmps);
	CHECK_EQ(u.port, 443);
	CHECK_EQ(u.path, std::string("rtmp/"));

	u = parseStreamUrl("srt://192.168.1.20:9000?streamid=abc&latency=200000");
	CHECK(u.valid);
	CHECK(u.protocol == Protocol::Srt);
	CHECK_EQ(u.port, 9000);
	CHECK_EQ(u.query, std::string("streamid=abc&latency=200000"));

	u = parseStreamUrl("srt://[fe80::1]:7001");
	CHECK(u.valid);
	CHECK_EQ(u.host, std::string("fe80::1"));
	CHECK_EQ(u.port, 7001);
}

TEST("url: invalid forms")
{
	CHECK(!parseStreamUrl("").valid);
	CHECK(!parseStreamUrl("a.rtmp.youtube.com/live2").valid);
	CHECK(!parseStreamUrl("http://example.com/live").valid);
	CHECK(!parseStreamUrl("rtmp:///live").valid);
	CHECK(!parseStreamUrl("rtmp://host:0/live").valid);
	CHECK(!parseStreamUrl("rtmp://host:70000/live").valid);
	CHECK(!parseStreamUrl("rtmp://host:/live").valid);
	CHECK(!parseStreamUrl("rtmp://host:12a/live").valid);
	CHECK(!parseStreamUrl("rtmp://ho st/live").valid);
	CHECK(!parseStreamUrl("rtmp://user:pass@host/live").valid);
	CHECK(!parseStreamUrl("rtmp://.host/live").valid);
	CHECK(!parseStreamUrl("srt://[fe80::1/x").valid);
	CHECK_EQ(parseStreamUrl("ftp://x").error, std::string("unsupported scheme"));
}

TEST("url: default ports and output types")
{
	CHECK_EQ(defaultPort(Protocol::Rtmp), 1935);
	CHECK_EQ(defaultPort(Protocol::Rtmps), 443);
	CHECK_EQ(defaultPort(Protocol::Srt), 0);
	CHECK_EQ(std::string(outputTypeFor(Protocol::Rtmp)), std::string("rtmp_output"));
	CHECK_EQ(std::string(outputTypeFor(Protocol::Rtmps)), std::string("rtmp_output"));
	CHECK_EQ(std::string(outputTypeFor(Protocol::Srt)), std::string("ffmpeg_mpegts_muxer"));
}

TEST("destination: platform ids round trip")
{
	for (Platform p : allPlatforms())
		CHECK(platformFromId(platformId(p)) == p);
	CHECK(platformFromId("myspace", Platform::CustomSrt) == Platform::CustomSrt);
}

TEST("destination: valid presets pass")
{
	CHECK(validate(valid(Platform::YouTube, "rtmps://a.rtmps.youtube.com:443/live2")).empty());
	CHECK(validate(valid(Platform::TikTok, "rtmp://push-rtmp-l11-sg01.tiktokcdn.com/game/")).empty());
	CHECK(validate(valid(Platform::Facebook, "rtmps://live-api-s.facebook.com:443/rtmp/")).empty());
	CHECK(validate(valid(Platform::Instagram, "rtmps://edgetee-upload-cdg4-2.xx.fbcdn.net:443/rtmp/")).empty());
	Destination srt = valid(Platform::CustomSrt, "srt://10.0.0.2:9000");
	srt.streamKey.clear(); /* optional for SRT */
	CHECK(validate(srt).empty());
}

TEST("destination: makeDestination prefills server and bitrate")
{
	const Destination yt = makeDestination(Platform::YouTube, "a", "YT");
	CHECK_EQ(yt.server, std::string("rtmps://a.rtmps.youtube.com:443/live2"));
	CHECK_EQ(yt.encoding.video.bitrateKbps, platformInfo(Platform::YouTube).recommendedKbps);
	CHECK(makeDestination(Platform::TikTok, "b", "TT").server.empty());
}

TEST("destination: errors")
{
	Destination d = valid(Platform::Instagram, "rtmp://edgetee-upload.fbcdn.net/rtmp/");
	d.name = "  ";
	d.streamKey.clear();
	auto v = validate(d);
	CHECK(hasIssue(v, IssueCode::NameEmpty));
	CHECK(hasIssue(v, IssueCode::ProtocolNotAllowed)); /* Instagram wants RTMPS */
	CHECK(hasIssue(v, IssueCode::KeyMissing));
	CHECK(hasErrors(v));

	d = valid(Platform::CustomSrt, "srt://10.0.0.2");
	CHECK(hasIssue(validate(d), IssueCode::SrtPortMissing));
	d = valid(Platform::CustomRtmp, "srt://10.0.0.2:9000");
	CHECK(hasIssue(validate(d), IssueCode::ProtocolNotAllowed));
	d = valid(Platform::CustomRtmp, "rtmp://host:99999/x");
	CHECK(hasIssue(validate(d), IssueCode::PortInvalid));
	d = valid(Platform::CustomRtmp, "");
	CHECK(hasIssue(validate(d), IssueCode::ServerEmpty));
	d = valid(Platform::CustomRtmp, "not a url");
	CHECK(hasIssue(validate(d), IssueCode::ServerInvalid));

	d = valid(Platform::TikTok, "rtmp://push.tiktokcdn.com/game/");
	d.encoding.video.codec = "hevc";
	CHECK(hasIssue(validate(d), IssueCode::CodecNotAllowed));
	d.encoding.video.codec = "vp9";
	CHECK(hasIssue(validate(d), IssueCode::CodecNotAllowed));

	d = valid(Platform::YouTube, "rtmp://a.rtmp.youtube.com/live2");
	d.encoding.video.bitrateKbps = 50;
	d.encoding.video.keyintSec = 30;
	d.encoding.audio.track = 7;
	d.encoding.audio.bitrateKbps = 8;
	v = validate(d);
	CHECK(hasIssue(v, IssueCode::BitrateInvalid));
	CHECK(hasIssue(v, IssueCode::KeyintInvalid));
	CHECK(hasIssue(v, IssueCode::AudioTrackInvalid));
	CHECK(hasIssue(v, IssueCode::AudioBitrateInvalid));
}

TEST("destination: warnings do not block")
{
	Destination d = valid(Platform::TikTok, "rtmp://push.tiktokcdn.com/game/");
	d.encoding.video.bitrateKbps = 12000;
	d.encoding.video.keyintSec = 4;
	d.streamKey = "abc def ghijklmnop";
	const auto v = validate(d);
	CHECK(hasIssue(v, IssueCode::BitrateAboveLimit));
	CHECK(hasIssue(v, IssueCode::KeyintNotRecommended));
	CHECK(hasIssue(v, IssueCode::KeyHasSpaces));
	CHECK(!hasErrors(v));
	/* Custom servers have no keyframe recommendation. */
	Destination c = valid(Platform::CustomRtmp, "rtmp://host/app");
	c.encoding.video.keyintSec = 4;
	CHECK(validate(c).empty());
}

TEST("destination: issue keys are locale keys")
{
	CHECK_EQ(std::string(issueKey(IssueCode::ServerInvalid)), std::string("Issue.ServerInvalid"));
	CHECK_EQ(std::string(issueKey(IssueCode::SrtPortMissing)), std::string("Issue.SrtPortMissing"));
}

TEST("destination: redactKey never shows the middle")
{
	CHECK_EQ(redactKey(""), std::string(""));
	CHECK_EQ(redactKey("short"), std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"));
	const std::string r = redactKey("live_123456789_abcdef");
	CHECK_EQ(r, std::string("live\xE2\x80\xA6" "cdef"));
	CHECK(r.find("123456789") == std::string::npos);
}

TEST("destination: makeId is unique and stable")
{
	std::vector<std::string> taken;
	for (int i = 0; i < 50; i++)
		taken.push_back(makeId(42, taken));
	std::vector<std::string> sorted = taken;
	std::sort(sorted.begin(), sorted.end());
	CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
	CHECK_EQ(makeId(7, {}), makeId(7, {}));
	CHECK_EQ(makeId(7, {}).size(), 12u);
}

TEST("encoding: key equality follows settings")
{
	EncoderSettings a, b;
	CHECK_EQ(a.key(), b.key());
	b.video.bitrateKbps = 4500;
	CHECK(a.key() != b.key());
	CHECK_EQ(a.audioKey(), b.audioKey());
	b = a;
	b.audio.track = 2;
	CHECK_EQ(a.videoKey(), b.videoKey());
	CHECK(a.audioKey() != b.audioKey());
	b = a;
	b.video.bframes = false;
	CHECK(!(a == b));
}
