#include "test.hpp"

#include "encoders.hpp"

using namespace vertical::core;

namespace {

const EncoderCandidate kAveAvc{"com.apple.videotoolbox.videoencoder.ave.avc", "h264", "Apple VT H264 Hardware Encoder"};
const EncoderCandidate kAveHevc{"com.apple.videotoolbox.videoencoder.ave.hevc", "hevc", "Apple VT HEVC Hardware Encoder"};
const EncoderCandidate kVtSw{"com.apple.videotoolbox.videoencoder.h264", "h264", "Apple VT H264 Software Encoder"};
const EncoderCandidate kGva{"com.apple.videotoolbox.videoencoder.h264.gva", "h264", "Apple VT H264 Hardware Encoder"};
const EncoderCandidate kX264{"obs_x264", "h264", "x264"};
const EncoderCandidate kNvenc{"obs_nvenc_h264_tex", "h264", "NVIDIA NVENC H.264"};
const EncoderCandidate kProRes{"com.apple.videotoolbox.videoencoder.prores.hw", "prores", "Apple VT ProRes Hardware Encoder"};

EncoderUser user(const std::string &id, int vkbps, int akbps = 160, int track = 1)
{
	EncoderUser u;
	u.id = id;
	u.settings.video.bitrateKbps = vkbps;
	u.settings.audio.bitrateKbps = akbps;
	u.settings.audio.track = track;
	return u;
}

} // namespace

TEST("encoders: identical settings share one video and one audio encoder")
{
	const EncoderPlan p = planEncoders({user("tiktok", 6000), user("youtube", 6000), user("insta", 4000)});
	CHECK_EQ(p.video.size(), 2u);
	CHECK_EQ(p.audio.size(), 1u);
	CHECK_EQ(p.video[0].users.size(), 2u);
	CHECK_EQ(p.video[0].users[0], std::string("tiktok"));
	CHECK_EQ(p.video[0].users[1], std::string("youtube"));
	CHECK_EQ(p.video[1].users[0], std::string("insta"));
	CHECK_EQ(p.audio[0].users.size(), 3u);
	CHECK_EQ(p.videoGroupOf("youtube"), 0);
	CHECK_EQ(p.videoGroupOf("insta"), 1);
	CHECK_EQ(p.audioGroupOf("insta"), 0);
	CHECK_EQ(p.videoGroupOf("nope"), -1);
}

TEST("encoders: audio groups split on bitrate or track only")
{
	const EncoderPlan p = planEncoders({user("a", 6000, 160, 1), user("b", 6000, 128, 1), user("c", 6000, 160, 2)});
	CHECK_EQ(p.video.size(), 1u);
	CHECK_EQ(p.audio.size(), 3u);
	CHECK_EQ(p.audioGroupOf("c"), 2);
}

TEST("encoders: empty plan")
{
	const EncoderPlan p = planEncoders({});
	CHECK(p.video.empty());
	CHECK(p.audio.empty());
}

TEST("encoders: destination groups skip disabled ones")
{
	std::vector<Destination> d(4);
	for (size_t i = 0; i < d.size(); i++)
		d[i].id = "d" + std::to_string(i);
	d[1].encoding.video.bitrateKbps = 4000;
	d[2].enabled = false;
	d[3].encoding.audio.bitrateKbps = 128; /* other audio: other full group */
	const auto g = groupDestinations(d);
	CHECK_EQ(g.size(), 3u);
	CHECK_EQ(g[0].destinationIds.size(), 1u);
	CHECK_EQ(g[0].destinationIds[0], std::string("d0"));
	CHECK_EQ(g[2].destinationIds[0], std::string("d3"));
	d[3].encoding = d[0].encoding;
	CHECK_EQ(groupDestinations(d).size(), 2u);
	CHECK_EQ(groupDestinations(d)[0].destinationIds.size(), 2u);
}

TEST("encoders: hardware detection")
{
	CHECK(isHardwareEncoder(kAveAvc));
	CHECK(isHardwareEncoder(kAveHevc));
	CHECK(isHardwareEncoder(kGva));
	CHECK(!isHardwareEncoder(kVtSw));
	CHECK(!isHardwareEncoder(kX264));
	CHECK(isHardwareEncoder(kNvenc));
	CHECK(isHardwareEncoder({"h264_texture_amf", "h264", "AMD"}));
	CHECK(isHardwareEncoder({"obs_qsv11_v2", "h264", "QuickSync"}));
}

TEST("encoders: auto picks VideoToolbox hardware on Apple silicon")
{
	VideoEncoding w;
	const EncoderChoice c = chooseVideoEncoder({kX264, kVtSw, kProRes, kAveHevc, kAveAvc}, w);
	CHECK_EQ(c.id, kAveAvc.id);
	CHECK(c.hardware);
	CHECK(!c.codecChanged);
	w.codec = "hevc";
	CHECK_EQ(chooseVideoEncoder({kX264, kAveAvc, kAveHevc}, w).id, kAveHevc.id);
}

TEST("encoders: Intel Mac and other hardware")
{
	VideoEncoding w;
	CHECK_EQ(chooseVideoEncoder({kX264, kVtSw, kGva}, w).id, kGva.id);
	CHECK_EQ(chooseVideoEncoder({kX264, kNvenc}, w).id, kNvenc.id);
}

TEST("encoders: hevc without hardware hevc falls back to hardware h264")
{
	VideoEncoding w;
	w.codec = "hevc";
	const EncoderChoice c = chooseVideoEncoder({kX264, kAveAvc}, w);
	CHECK_EQ(c.id, kAveAvc.id);
	CHECK(c.codecChanged);
	CHECK_EQ(c.codec, std::string("h264"));
}

TEST("encoders: x264 only when there is no hardware encoder")
{
	VideoEncoding w;
	const EncoderChoice c = chooseVideoEncoder({kVtSw, kX264}, w);
	CHECK_EQ(c.id, std::string("obs_x264"));
	CHECK(!c.hardware);
	CHECK(chooseVideoEncoder({kVtSw}, w).id.empty());
	CHECK(chooseVideoEncoder({}, w).id.empty());
}

TEST("encoders: explicit id is honoured, missing one falls back to auto")
{
	VideoEncoding w;
	w.encoder = "obs_x264";
	EncoderChoice c = chooseVideoEncoder({kX264, kAveAvc}, w);
	CHECK_EQ(c.id, std::string("obs_x264"));
	CHECK(!c.requestedMissing);
	w.encoder = "jim_nvenc";
	c = chooseVideoEncoder({kX264, kAveAvc}, w);
	CHECK_EQ(c.id, kAveAvc.id);
	CHECK(c.requestedMissing);
}

TEST("encoders: audio preference")
{
	CHECK_EQ(chooseAudioEncoder({"ffmpeg_aac", "CoreAudio_AAC", "ffmpeg_opus"}), std::string("CoreAudio_AAC"));
	CHECK_EQ(chooseAudioEncoder({"ffmpeg_opus", "ffmpeg_aac"}), std::string("ffmpeg_aac"));
	CHECK(chooseAudioEncoder({"ffmpeg_opus"}).empty());
}
