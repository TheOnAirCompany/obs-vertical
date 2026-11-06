#pragma once

/* Recording and replay buffer settings of the vertical output. Pure C++17. */

#include "destination.hpp"

#include <string>
#include <vector>

namespace vertical::core {

enum class RecordFormat { HybridMp4, Mp4, Mkv, Mov, HybridMov };

const char *recordFormatId(RecordFormat f); /* "hybrid_mp4", "mp4", "mkv", "mov", "hybrid_mov" */
RecordFormat recordFormatFromId(const std::string &id, RecordFormat fallback = RecordFormat::HybridMp4);

struct RecordingSettings {
	std::string path; /* folder; empty = the main OBS recording folder */
	RecordFormat format = RecordFormat::HybridMp4;
	std::string filenameFormat = "Vertical %CCYY-%MM-%DD %hh-%mm-%ss";
	EncoderSettings encoding;

	RecordingSettings()
	{
		encoding.video.bitrateKbps = 12000;
		encoding.video.rateControl = "CBR";
	}
};

struct ReplaySettings {
	bool enabled = false; /* the replay buffer starts with the first output */
	int seconds = 30;
	int maxMb = 512;
};

struct MuxerChoice {
	std::string outputType; /* "mp4_output", "mov_output", "ffmpeg_muxer" */
	std::string extension;  /* without the dot */
	bool hybrid = false;
	bool fellBack = false; /* hybrid asked but not available: plain muxer used */
};

/* Picks the OBS output type for a format given the output types registered at
 * runtime. Hybrid MP4/MOV (mp4_output/mov_output, OBS 30.2+) survive a crash;
 * without them, MKV is used for "hybrid_mp4" because a plain MP4 is lost when
 * the recording is cut. */
MuxerChoice chooseMuxer(RecordFormat f, const std::vector<std::string> &outputTypes);

/* The replay buffer always uses ffmpeg's replay_buffer output; it can write
 * mp4, mov or mkv. */
std::string replayExtension(RecordFormat f);

} // namespace vertical::core
