#include "audio-mix.hpp"

#include <algorithm>
#include <cctype>

namespace vertical::core {

uint32_t usedTracks(const ObsOutputConfig &c)
{
	uint32_t used = 0;
	const uint32_t all = (1u << kTrackCount) - 1;
	if (!c.advanced) {
		used |= trackBit(1);
		if (!c.simpleRecSameAsStream)
			used |= c.simpleRecTracks & all;
		if (c.simpleVodTrack)
			used |= trackBit(2);
		return used;
	}
	used |= trackBit(c.advStreamTrack);
	used |= (c.advRecFFmpeg ? c.advFFmpegTracks : c.advRecTracks) & all;
	if (c.advVodTrack)
		used |= trackBit(c.advVodTrackIndex);
	return used;
}

int chooseVerticalTrack(uint32_t used, int current)
{
	if (current >= 1 && current <= kTrackCount && !(used & trackBit(current)))
		return current;
	for (int t = kTrackCount; t >= 1; t--)
		if (!(used & trackBit(t)))
			return t;
	return kFallbackVerticalTrack;
}

uint32_t verticalMixers(uint32_t mixers, int verticalTrack, bool excluded, int mainTrack)
{
	const uint32_t bit = trackBit(verticalTrack);
	if (!bit || verticalTrack == mainTrack)
		return mixers;
	const bool on = (mixers & trackBit(mainTrack)) && !excluded;
	return on ? (mixers | bit) : (mixers & ~bit);
}

bool AudioExclusions::setExcluded(const std::string &uuid, bool excluded)
{
	if (uuid.empty())
		return false;
	if (excluded)
		return uuids_.insert(uuid).second;
	return uuids_.erase(uuid) != 0;
}

size_t AudioExclusions::prune(const std::vector<std::string> &existing)
{
	size_t dropped = 0;
	for (auto it = uuids_.begin(); it != uuids_.end();) {
		if (std::find(existing.begin(), existing.end(), *it) == existing.end()) {
			it = uuids_.erase(it);
			dropped++;
		} else {
			++it;
		}
	}
	return dropped;
}

Json AudioExclusions::toJson() const
{
	/* Objects, not bare strings: OBS keeps only objects in the arrays of obs_data. */
	Json a = Json::array();
	for (const std::string &u : uuids_) {
		Json o = Json::object();
		o.set("uuid", u);
		a.push(std::move(o));
	}
	return a;
}

AudioExclusions AudioExclusions::fromJson(const Json &j)
{
	AudioExclusions e;
	for (const Json &u : j.items()) {
		if (u.isString())
			e.setExcluded(u.asString(), true);
		else if (u.isObject())
			e.setExcluded(u.get("uuid").asString(), true);
	}
	return e;
}

namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

} // namespace

std::vector<AudioRow> audioRows(const std::vector<AudioSourceInfo> &sources, const AudioExclusions &exclusions,
				int mainTrack)
{
	std::vector<AudioRow> rows;
	for (const AudioSourceInfo &s : sources) {
		if (s.uuid.empty())
			continue;
		if (std::any_of(rows.begin(), rows.end(), [&](const AudioRow &r) { return r.uuid == s.uuid; }))
			continue;
		AudioRow r;
		r.uuid = s.uuid;
		r.name = s.name;
		r.sent = !exclusions.excluded(s.uuid);
		r.onMainMix = (s.mixers & trackBit(mainTrack)) != 0;
		r.global = s.global;
		rows.push_back(std::move(r));
	}
	std::stable_sort(rows.begin(), rows.end(), [](const AudioRow &a, const AudioRow &b) {
		if (a.global != b.global)
			return a.global;
		return lower(a.name) < lower(b.name);
	});
	return rows;
}

} // namespace vertical::core
