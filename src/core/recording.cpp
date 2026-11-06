#include "recording.hpp"

#include <algorithm>

namespace vertical::core {

const char *recordFormatId(RecordFormat f)
{
	switch (f) {
	case RecordFormat::HybridMp4:
		return "hybrid_mp4";
	case RecordFormat::Mp4:
		return "mp4";
	case RecordFormat::Mkv:
		return "mkv";
	case RecordFormat::Mov:
		return "mov";
	case RecordFormat::HybridMov:
		return "hybrid_mov";
	}
	return "hybrid_mp4";
}

RecordFormat recordFormatFromId(const std::string &id, RecordFormat fallback)
{
	for (RecordFormat f : {RecordFormat::HybridMp4, RecordFormat::Mp4, RecordFormat::Mkv, RecordFormat::Mov,
			       RecordFormat::HybridMov})
		if (id == recordFormatId(f))
			return f;
	return fallback;
}

MuxerChoice chooseMuxer(RecordFormat f, const std::vector<std::string> &types)
{
	const auto has = [&](const char *t) {
		return std::find(types.begin(), types.end(), t) != types.end();
	};
	MuxerChoice c;
	switch (f) {
	case RecordFormat::HybridMp4:
		if (has("mp4_output"))
			return {"mp4_output", "mp4", true, false};
		return {"ffmpeg_muxer", "mkv", false, true};
	case RecordFormat::HybridMov:
		if (has("mov_output"))
			return {"mov_output", "mov", true, false};
		return {"ffmpeg_muxer", "mkv", false, true};
	case RecordFormat::Mp4:
		return {"ffmpeg_muxer", "mp4", false, false};
	case RecordFormat::Mkv:
		return {"ffmpeg_muxer", "mkv", false, false};
	case RecordFormat::Mov:
		return {"ffmpeg_muxer", "mov", false, false};
	}
	return c;
}

std::string replayExtension(RecordFormat f)
{
	switch (f) {
	case RecordFormat::Mkv:
		return "mkv";
	case RecordFormat::Mov:
	case RecordFormat::HybridMov:
		return "mov";
	default:
		return "mp4";
	}
}

} // namespace vertical::core
