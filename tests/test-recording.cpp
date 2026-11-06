#include "test.hpp"

#include "recording.hpp"

using namespace vertical::core;

TEST("recording: format ids round trip")
{
	for (RecordFormat f :
	     {RecordFormat::HybridMp4, RecordFormat::Mp4, RecordFormat::Mkv, RecordFormat::Mov, RecordFormat::HybridMov})
		CHECK(recordFormatFromId(recordFormatId(f)) == f);
	CHECK(recordFormatFromId("avi", RecordFormat::Mkv) == RecordFormat::Mkv);
}

TEST("recording: hybrid muxers when available")
{
	const std::vector<std::string> obs33{"rtmp_output", "ffmpeg_muxer", "mp4_output", "mov_output", "replay_buffer"};
	MuxerChoice c = chooseMuxer(RecordFormat::HybridMp4, obs33);
	CHECK_EQ(c.outputType, std::string("mp4_output"));
	CHECK_EQ(c.extension, std::string("mp4"));
	CHECK(c.hybrid);
	CHECK(!c.fellBack);
	c = chooseMuxer(RecordFormat::HybridMov, obs33);
	CHECK_EQ(c.outputType, std::string("mov_output"));
	c = chooseMuxer(RecordFormat::Mkv, obs33);
	CHECK_EQ(c.outputType, std::string("ffmpeg_muxer"));
	CHECK_EQ(c.extension, std::string("mkv"));
}

TEST("recording: without hybrid, MKV keeps the file safe")
{
	const MuxerChoice c = chooseMuxer(RecordFormat::HybridMp4, {"ffmpeg_muxer"});
	CHECK_EQ(c.outputType, std::string("ffmpeg_muxer"));
	CHECK_EQ(c.extension, std::string("mkv"));
	CHECK(c.fellBack);
	CHECK_EQ(chooseMuxer(RecordFormat::Mp4, {"ffmpeg_muxer"}).extension, std::string("mp4"));
}

TEST("recording: replay extensions")
{
	CHECK_EQ(replayExtension(RecordFormat::HybridMp4), std::string("mp4"));
	CHECK_EQ(replayExtension(RecordFormat::Mkv), std::string("mkv"));
	CHECK_EQ(replayExtension(RecordFormat::HybridMov), std::string("mov"));
}
