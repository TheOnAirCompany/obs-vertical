#include "settings.hpp"

#include <algorithm>

namespace vertical::core {

namespace {

void warn(std::vector<std::string> *w, const std::string &msg)
{
	if (w)
		w->push_back(msg);
}

int clampInt(const Json &j, int fallback, int lo, int hi, const char *what, std::vector<std::string> *w)
{
	if (j.isNull())
		return fallback;
	const int v = j.asInt(fallback);
	if (!j.isNumber() || v < lo || v > hi) {
		warn(w, std::string("invalid ") + what);
		return fallback;
	}
	return v;
}

} // namespace

const Destination *Settings::findDestination(const std::string &id) const
{
	for (const Destination &d : destinations)
		if (d.id == id)
			return &d;
	return nullptr;
}

Destination *Settings::findDestination(const std::string &id)
{
	for (Destination &d : destinations)
		if (d.id == id)
			return &d;
	return nullptr;
}

std::vector<std::string> Settings::destinationIds() const
{
	std::vector<std::string> ids;
	for (const Destination &d : destinations)
		ids.push_back(d.id);
	return ids;
}

Json encodingToJson(const EncoderSettings &e)
{
	Json v = Json::object();
	v.set("encoder", e.video.encoder);
	v.set("codec", e.video.codec);
	v.set("bitrate", e.video.bitrateKbps);
	v.set("keyint_sec", e.video.keyintSec);
	v.set("rate_control", e.video.rateControl);
	v.set("profile", e.video.profile);
	v.set("bframes", e.video.bframes);
	Json a = Json::object();
	a.set("bitrate", e.audio.bitrateKbps);
	a.set("track", e.audio.track);
	Json o = Json::object();
	o.set("video", std::move(v));
	o.set("audio", std::move(a));
	return o;
}

EncoderSettings encodingFromJson(const Json &j)
{
	EncoderSettings e;
	const Json &v = j.get("video");
	const Json &a = j.get("audio");
	e.video.encoder = v.get("encoder").asString(e.video.encoder);
	e.video.codec = v.get("codec").asString(e.video.codec);
	e.video.bitrateKbps = v.get("bitrate").asInt(e.video.bitrateKbps);
	e.video.keyintSec = v.get("keyint_sec").asInt(e.video.keyintSec);
	e.video.rateControl = v.get("rate_control").asString(e.video.rateControl);
	e.video.profile = v.get("profile").asString(e.video.profile);
	e.video.bframes = v.get("bframes").asBool(e.video.bframes);
	e.audio.bitrateKbps = a.get("bitrate").asInt(e.audio.bitrateKbps);
	e.audio.track = a.get("track").asInt(e.audio.track);
	if (e.video.encoder.empty())
		e.video.encoder = "auto";
	return e;
}

Json destinationToJson(const Destination &d, KeyMode keys)
{
	Json o = Json::object();
	o.set("id", d.id);
	o.set("name", d.name);
	o.set("platform", platformId(d.platform));
	o.set("server", d.server);
	o.set("key", keys == KeyMode::Include ? d.streamKey : redactKey(d.streamKey));
	o.set("enabled", d.enabled);
	o.set("encoding", encodingToJson(d.encoding));
	return o;
}

Destination destinationFromJson(const Json &j, std::vector<std::string> *w)
{
	Destination d;
	d.id = j.get("id").asString();
	d.name = j.get("name").asString();
	const std::string pid = j.get("platform").asString();
	d.platform = platformFromId(pid, Platform::CustomRtmp);
	if (!pid.empty() && pid != platformId(d.platform))
		warn(w, "unknown platform " + pid);
	d.server = j.get("server").asString();
	d.streamKey = j.get("key").asString();
	d.enabled = j.get("enabled").asBool(true);
	if (j.has("encoding"))
		d.encoding = encodingFromJson(j.get("encoding"));
	else
		d.encoding.video.bitrateKbps = platformInfo(d.platform).recommendedKbps;
	return d;
}

Json settingsToJson(const Settings &s, KeyMode keys)
{
	Json o = Json::object();
	o.set("version", kSettingsVersion);

	Json c = Json::object();
	c.set("name", s.canvas.name);
	c.set("uuid", s.canvas.uuid);
	c.set("width", s.canvas.size.width);
	c.set("height", s.canvas.size.height);
	c.set("output_height", s.canvas.outputHeight);
	o.set("canvas", std::move(c));

	/* Objects, not bare strings: OBS keeps only objects in the arrays of obs_data
	 * (the collection file), bare strings were lost. */
	Json order = Json::array();
	for (const std::string &n : s.sceneOrder) {
		Json o = Json::object();
		o.set("name", n);
		order.push(std::move(o));
	}
	o.set("scene_order", std::move(order));
	o.set("current_scene", s.currentScene);
	o.set("linked_scenes", s.linkedScenes);
	o.set("links", s.links.toJson());

	Json dests = Json::array();
	for (const Destination &d : s.destinations)
		dests.push(destinationToJson(d, keys));
	o.set("destinations", std::move(dests));

	Json r = Json::object();
	r.set("path", s.recording.path);
	r.set("format", recordFormatId(s.recording.format));
	r.set("filename_format", s.recording.filenameFormat);
	r.set("encoding", encodingToJson(s.recording.encoding));
	o.set("recording", std::move(r));

	Json rb = Json::object();
	rb.set("enabled", s.replay.enabled);
	rb.set("seconds", s.replay.seconds);
	rb.set("max_mb", s.replay.maxMb);
	o.set("replay", std::move(rb));

	o.set("preview_visible", s.previewVisible);
	return o;
}

Settings settingsFromJson(const Json &j, std::vector<std::string> *w)
{
	Settings s;
	if (!j.isObject()) {
		if (!j.isNull())
			warn(w, "settings are not an object");
		return s;
	}
	const int version = j.get("version").asInt(kSettingsVersion);
	if (version > kSettingsVersion)
		warn(w, "settings saved by a newer version (" + std::to_string(version) + ")");

	const Json &c = j.get("canvas");
	s.canvas.name = c.get("name").asString(s.canvas.name);
	if (s.canvas.name.empty())
		s.canvas.name = "Vertical";
	s.canvas.uuid = c.get("uuid").asString();
	const Size size{c.get("width").asInt(0), c.get("height").asInt(0)};
	if (c.has("width") || c.has("height")) {
		if (isSizeError(checkCanvasSize(size)))
			warn(w, "invalid canvas size " + formatSize(size));
		else
			s.canvas.size = size;
	}
	s.canvas.outputHeight = clampInt(c.get("output_height"), 0, 0, kMaxCanvasSide, "output height", w);

	for (const Json &n : j.get("scene_order").items()) {
		const std::string name = n.isObject() ? n.get("name").asString() : n.asString("");
		if (!name.empty())
			s.sceneOrder.push_back(name);
	}
	s.currentScene = j.get("current_scene").asString();
	s.linkedScenes = j.get("linked_scenes").asBool(false);
	s.links = SceneLinks::fromJson(j.get("links"));

	std::vector<std::string> ids;
	for (const Json &dj : j.get("destinations").items()) {
		Destination d = destinationFromJson(dj, w);
		if (d.id.empty() || std::find(ids.begin(), ids.end(), d.id) != ids.end()) {
			/* Missing or duplicated id: give it a new one so hotkeys and
			 * status stay per destination. */
			d.id = makeId(ids.size() + 0x5eedull, ids);
			warn(w, "destination without a unique id, new id " + d.id);
		}
		ids.push_back(d.id);
		s.destinations.push_back(std::move(d));
	}

	const Json &r = j.get("recording");
	s.recording.path = r.get("path").asString();
	const std::string fmt = r.get("format").asString();
	s.recording.format = recordFormatFromId(fmt, RecordFormat::HybridMp4);
	if (!fmt.empty() && fmt != recordFormatId(s.recording.format))
		warn(w, "unknown recording format " + fmt);
	s.recording.filenameFormat = r.get("filename_format").asString(s.recording.filenameFormat);
	if (s.recording.filenameFormat.empty())
		s.recording.filenameFormat = RecordingSettings().filenameFormat;
	if (r.has("encoding"))
		s.recording.encoding = encodingFromJson(r.get("encoding"));

	const Json &rb = j.get("replay");
	s.replay.enabled = rb.get("enabled").asBool(false);
	s.replay.seconds = clampInt(rb.get("seconds"), s.replay.seconds, 5, 21600, "replay length", w);
	s.replay.maxMb = clampInt(rb.get("max_mb"), s.replay.maxMb, 16, 65536, "replay size", w);

	s.previewVisible = j.get("preview_visible").asBool(true);
	return s;
}

std::string saveSettings(const Settings &s, KeyMode keys, bool pretty)
{
	return settingsToJson(s, keys).dump(pretty);
}

Settings loadSettings(const std::string &text, std::vector<std::string> *w)
{
	if (text.empty())
		return Settings();
	std::string error;
	const Json j = Json::parse(text, &error);
	if (!error.empty()) {
		warn(w, "settings JSON: " + error);
		return Settings();
	}
	return settingsFromJson(j, w);
}

} // namespace vertical::core
