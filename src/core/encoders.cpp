#include "encoders.hpp"

#include <algorithm>

namespace vertical::core {

namespace {

bool contains(const std::string &s, const char *part)
{
	return s.find(part) != std::string::npos;
}

bool endsWith(const std::string &s, const std::string &suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

const EncoderCandidate *findId(const std::vector<EncoderCandidate> &list, const std::string &id)
{
	for (const EncoderCandidate &c : list)
		if (c.id == id)
			return &c;
	return nullptr;
}

/* Lower is better. Known first-choice ids come before generic hardware ones. */
int rank(const EncoderCandidate &c, const std::string &codec)
{
	if (c.codec != codec)
		return 1000;
	if (c.id == "com.apple.videotoolbox.videoencoder.ave.avc" ||
	    c.id == "com.apple.videotoolbox.videoencoder.ave.hevc")
		return 0;
	if (contains(c.id, "videotoolbox") && isHardwareEncoder(c))
		return 10;
	if (isHardwareEncoder(c))
		return 20;
	return 1000;
}

const EncoderCandidate *bestHardware(const std::vector<EncoderCandidate> &list, const std::string &codec)
{
	const EncoderCandidate *best = nullptr;
	int bestRank = 1000;
	for (const EncoderCandidate &c : list) {
		const int r = rank(c, codec);
		if (r < bestRank) {
			best = &c;
			bestRank = r;
		}
	}
	return best;
}

} // namespace

int EncoderPlan::videoGroupOf(const std::string &userId) const
{
	for (size_t i = 0; i < video.size(); i++)
		if (std::find(video[i].users.begin(), video[i].users.end(), userId) != video[i].users.end())
			return (int)i;
	return -1;
}

int EncoderPlan::audioGroupOf(const std::string &userId) const
{
	for (size_t i = 0; i < audio.size(); i++)
		if (std::find(audio[i].users.begin(), audio[i].users.end(), userId) != audio[i].users.end())
			return (int)i;
	return -1;
}

EncoderPlan planEncoders(const std::vector<EncoderUser> &users)
{
	EncoderPlan plan;
	for (const EncoderUser &u : users) {
		const std::string vk = u.settings.videoKey();
		auto v = std::find_if(plan.video.begin(), plan.video.end(),
				      [&](const VideoGroup &g) { return g.key == vk; });
		if (v == plan.video.end())
			plan.video.push_back({vk, u.settings.video, {u.id}});
		else
			v->users.push_back(u.id);

		const std::string ak = u.settings.audioKey();
		auto a = std::find_if(plan.audio.begin(), plan.audio.end(),
				      [&](const AudioGroup &g) { return g.key == ak; });
		if (a == plan.audio.end())
			plan.audio.push_back({ak, u.settings.audio, {u.id}});
		else
			a->users.push_back(u.id);
	}
	return plan;
}

std::vector<DestinationGroup> groupDestinations(const std::vector<Destination> &destinations)
{
	std::vector<DestinationGroup> groups;
	for (const Destination &d : destinations) {
		if (!d.enabled)
			continue;
		const std::string k = d.encoding.key();
		auto g = std::find_if(groups.begin(), groups.end(),
				      [&](const DestinationGroup &x) { return x.key == k; });
		if (g == groups.end())
			groups.push_back({k, d.encoding, {d.id}});
		else
			g->destinationIds.push_back(d.id);
	}
	return groups;
}

bool isHardwareEncoder(const EncoderCandidate &c)
{
	const std::string &id = c.id;
	const std::string &name = c.displayName;
	if (contains(id, "videotoolbox")) {
		/* Apple silicon: ".ave."; Intel Macs: ".gva" (H.264) and "(HW)" names.
		 * The display names of the OBS VideoToolbox module say Hardware/Software. */
		if (contains(name, "Software") || contains(name, "(SW)"))
			return false;
		return contains(id, ".ave.") || endsWith(id, ".gva") || contains(name, "Hardware") ||
		       contains(name, "(HW)");
	}
	return contains(id, "nvenc") || contains(id, "amf") || contains(id, "qsv") || contains(id, "vaapi") ||
	       contains(id, "_va_") || contains(id, "v4l2m2m");
}

EncoderChoice chooseVideoEncoder(const std::vector<EncoderCandidate> &available, const VideoEncoding &wanted)
{
	EncoderChoice choice;
	if (wanted.encoder != "auto" && !wanted.encoder.empty()) {
		if (const EncoderCandidate *c = findId(available, wanted.encoder)) {
			choice.id = c->id;
			choice.codec = c->codec;
			choice.hardware = isHardwareEncoder(*c);
			return choice;
		}
		choice.requestedMissing = true;
	}

	const std::string codec = wanted.codec.empty() ? "h264" : wanted.codec;
	if (const EncoderCandidate *c = bestHardware(available, codec)) {
		choice.id = c->id;
		choice.codec = c->codec;
		choice.hardware = true;
		return choice;
	}
	if (codec != "h264") {
		/* A hardware H.264 encoder costs less CPU than a software HEVC one. */
		if (const EncoderCandidate *c = bestHardware(available, "h264")) {
			choice.id = c->id;
			choice.codec = c->codec;
			choice.hardware = true;
			choice.codecChanged = true;
			return choice;
		}
	}
	if (const EncoderCandidate *c = findId(available, "obs_x264")) {
		choice.id = c->id;
		choice.codec = "h264";
		choice.hardware = false;
		choice.codecChanged = codec != "h264";
		return choice;
	}
	return choice;
}

std::string chooseAudioEncoder(const std::vector<std::string> &ids)
{
	for (const char *pref : {"CoreAudio_AAC", "ffmpeg_aac", "libfdk_aac"})
		if (std::find(ids.begin(), ids.end(), pref) != ids.end())
			return pref;
	return {};
}

} // namespace vertical::core
