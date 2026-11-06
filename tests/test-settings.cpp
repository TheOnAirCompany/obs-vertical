#include "test.hpp"

#include "settings.hpp"

using namespace vertical::core;

namespace {

Settings sample()
{
	Settings s;
	s.canvas.uuid = "1d2c3b4a";
	s.canvas.size = {720, 1280};
	s.canvas.outputHeight = 960;
	s.sceneOrder = {"V Cam", "V Game"};
	s.currentScene = "V Game";
	s.linkedScenes = true;
	s.links.link("Cam", "V Cam");
	Destination tt = makeDestination(Platform::TikTok, "a1", "TikTok");
	tt.server = "rtmp://push.tiktokcdn.com/game/";
	tt.streamKey = "stream-key-0123456789";
	Destination yt = makeDestination(Platform::YouTube, "b2", "YouTube");
	yt.streamKey = "yt-key-abcdefghij";
	yt.enabled = false;
	yt.encoding.video.codec = "hevc";
	yt.encoding.audio.track = 2;
	s.destinations = {tt, yt};
	s.recording.path = "/Users/me/Movies";
	s.recording.format = RecordFormat::Mkv;
	s.replay.enabled = true;
	s.replay.seconds = 45;
	s.previewVisible = false;
	return s;
}

} // namespace

TEST("settings: defaults")
{
	const Settings s;
	CHECK(s.canvas.size == (Size{1080, 1920}));
	CHECK_EQ(s.canvas.name, std::string("Vertical"));
	CHECK(s.destinations.empty());
	CHECK(s.recording.format == RecordFormat::HybridMp4);
	CHECK(!s.replay.enabled);
	CHECK(s.previewVisible);
	CHECK(!s.linkedScenes);
}

TEST("settings: round trip keeps everything")
{
	const Settings s = sample();
	std::vector<std::string> warnings;
	const Settings b = loadSettings(saveSettings(s), &warnings);
	CHECK(warnings.empty());
	CHECK_EQ(b.canvas.uuid, s.canvas.uuid);
	CHECK(b.canvas.size == s.canvas.size);
	CHECK_EQ(b.canvas.outputHeight, 960);
	CHECK(b.sceneOrder == s.sceneOrder);
	CHECK_EQ(b.currentScene, std::string("V Game"));
	CHECK(b.linkedScenes);
	CHECK(b.links == s.links);
	CHECK_EQ(b.destinations.size(), 2u);
	CHECK_EQ(b.destinations[0].streamKey, std::string("stream-key-0123456789"));
	CHECK(b.destinations[1].platform == Platform::YouTube);
	CHECK(!b.destinations[1].enabled);
	CHECK(b.destinations[1].encoding == s.destinations[1].encoding);
	CHECK_EQ(b.recording.path, std::string("/Users/me/Movies"));
	CHECK(b.recording.format == RecordFormat::Mkv);
	CHECK(b.recording.encoding == s.recording.encoding);
	CHECK(b.replay.enabled);
	CHECK_EQ(b.replay.seconds, 45);
	CHECK(!b.previewVisible);
	/* Saving again gives the same text: stable files. */
	CHECK_EQ(saveSettings(b), saveSettings(s));
}

TEST("settings: redacted export hides keys")
{
	const std::string text = saveSettings(sample(), KeyMode::Redact, true);
	CHECK(text.find("stream-key-0123456789") == std::string::npos);
	CHECK(text.find("yt-key-abcdefghij") == std::string::npos);
	CHECK(text.find("rtmp://push.tiktokcdn.com/game/") != std::string::npos);
}

TEST("settings: broken or empty input gives defaults with a warning")
{
	std::vector<std::string> w;
	Settings s = loadSettings("", &w);
	CHECK(w.empty());
	CHECK(s.canvas.size == defaultCanvasSize());
	s = loadSettings("{not json", &w);
	CHECK_EQ(w.size(), 1u);
	CHECK(s.destinations.empty());
	w.clear();
	s = loadSettings("[1,2]", &w);
	CHECK_EQ(w.size(), 1u);
}

TEST("settings: invalid values fall back one by one")
{
	std::vector<std::string> w;
	const Settings s = loadSettings(R"({
		"version": 1,
		"canvas": {"width": 1081, "height": 1920, "output_height": -5},
		"destinations": [
			{"id": "x", "name": "A", "platform": "myspace", "server": "rtmp://h/a", "key": "k"},
			{"id": "x", "name": "B", "platform": "youtube"},
			{"name": "C", "platform": "srt", "server": "srt://h:1"}
		],
		"recording": {"format": "avi"},
		"replay": {"seconds": 1, "max_mb": "big"},
		"unknown": {"ignored": true}
	})",
					&w);
	CHECK(s.canvas.size == defaultCanvasSize());
	CHECK_EQ(s.canvas.outputHeight, 0);
	CHECK_EQ(s.destinations.size(), 3u);
	CHECK(s.destinations[0].platform == Platform::CustomRtmp);
	CHECK(s.destinations[0].id != s.destinations[1].id);
	CHECK(!s.destinations[2].id.empty());
	CHECK(s.destinations[2].id != s.destinations[1].id);
	CHECK(s.destinations[1].encoding.video.bitrateKbps == platformInfo(Platform::YouTube).recommendedKbps);
	CHECK(s.recording.format == RecordFormat::HybridMp4);
	CHECK_EQ(s.replay.seconds, 30);
	CHECK_EQ(s.replay.maxMb, 512);
	/* canvas size, output height, platform, 2 ids, format, replay length, replay size */
	CHECK_EQ(w.size(), 8u);
}

TEST("settings: newer version is read with a warning")
{
	std::vector<std::string> w;
	const Settings s = loadSettings(R"({"version": 9, "linked_scenes": true})", &w);
	CHECK(s.linkedScenes);
	CHECK_EQ(w.size(), 1u);
}

TEST("settings: find destinations")
{
	Settings s = sample();
	CHECK(s.findDestination("b2") != nullptr);
	CHECK(s.findDestination("zz") == nullptr);
	s.findDestination("a1")->name = "TT";
	CHECK_EQ(s.destinations[0].name, std::string("TT"));
	CHECK_EQ(s.destinationIds().size(), 2u);
}

namespace {

/* OBS stores the settings in an obs_data: its arrays keep objects only. */
bool arraysHoldObjectsOnly(const Json &j)
{
	if (j.isArray()) {
		for (const Json &item : j.items())
			if (!item.isObject() || !arraysHoldObjectsOnly(item))
				return false;
		return true;
	}
	if (j.isObject())
		for (const auto &m : j.members())
			if (!arraysHoldObjectsOnly(m.second))
				return false;
	return true;
}

} // namespace

TEST("settings: every array holds objects (obs_data drops bare values)")
{
	Settings s;
	s.sceneOrder = {"A", "B"};
	s.links.link("Main", "A");
	s.destinations.push_back(makeDestination(Platform::TikTok, "d1", "TikTok"));
	const Json j = settingsToJson(s);
	CHECK(arraysHoldObjectsOnly(j));
	const Settings back = settingsFromJson(j);
	CHECK(back.sceneOrder == s.sceneOrder);
	/* Older files: bare strings are still read. */
	const Settings old = loadSettings("{\"version\":1,\"scene_order\":[\"X\",\"Y\"]}");
	CHECK(old.sceneOrder == std::vector<std::string>({"X", "Y"}));
}
