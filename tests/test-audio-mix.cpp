#include "test.hpp"

#include "audio-mix.hpp"
#include "settings.hpp"

using namespace vertical::core;

TEST("audio: OBS simple mode uses track 1 only")
{
	ObsOutputConfig c;
	CHECK_EQ(usedTracks(c), trackBit(1));
	c.simpleRecSameAsStream = false;
	c.simpleRecTracks = 0b000011;
	CHECK_EQ(usedTracks(c), 0b000011u);
	c.simpleVodTrack = true;
	c.simpleRecTracks = 1;
	CHECK_EQ(usedTracks(c), 0b000011u);
}

TEST("audio: OBS advanced mode uses its stream, recording and VOD tracks")
{
	ObsOutputConfig c;
	c.advanced = true;
	c.advStreamTrack = 2;
	c.advRecTracks = 0b000101;
	CHECK_EQ(usedTracks(c), 0b000111u);
	c.advRecFFmpeg = true;
	c.advFFmpegTracks = 0b100000;
	CHECK_EQ(usedTracks(c), 0b100010u);
	c.advVodTrack = true;
	c.advVodTrackIndex = 5;
	CHECK_EQ(usedTracks(c), 0b110010u);
	c.advRecTracks = 0xffffffff; /* bits above track 6 are ignored */
	c.advRecFFmpeg = false;
	CHECK_EQ(usedTracks(c), 0b111111u);
}

TEST("audio: the vertical track is the first free track from 6 down")
{
	CHECK_EQ(chooseVerticalTrack(trackBit(1), 0), 6);
	CHECK_EQ(chooseVerticalTrack(trackBit(1) | trackBit(6), 0), 5);
	CHECK_EQ(chooseVerticalTrack(0b011111, 0), 6);
	CHECK_EQ(chooseVerticalTrack(0b111111, 0), kFallbackVerticalTrack); /* all taken */
	/* A track already chosen stays while it is free. */
	CHECK_EQ(chooseVerticalTrack(trackBit(1), 4), 4);
	CHECK_EQ(chooseVerticalTrack(trackBit(1) | trackBit(4), 4), 6);
	CHECK_EQ(chooseVerticalTrack(trackBit(1), 9), 6);
}

TEST("audio: vertical bit = on the main track AND not excluded, other bits untouched")
{
	/* on track 1, included -> on track 6 */
	CHECK_EQ(verticalMixers(0b000001, 6, false), 0b100001u);
	/* on track 1, excluded -> off track 6 */
	CHECK_EQ(verticalMixers(0b100001, 6, true), 0b000001u);
	/* not on track 1 -> off track 6 even when included */
	CHECK_EQ(verticalMixers(0b100010, 6, false), 0b000010u);
	/* other tracks are never changed */
	CHECK_EQ(verticalMixers(0b011111, 6, false), 0b111111u);
	CHECK_EQ(verticalMixers(0b011110, 6, false), 0b011110u);
	CHECK_EQ(verticalMixers(0b111111, 6, true), 0b011111u);
	/* another vertical track */
	CHECK_EQ(verticalMixers(0b000001, 3, false), 0b000101u);
	/* the main track itself or an invalid track: unchanged */
	CHECK_EQ(verticalMixers(0b000001, 1, true), 0b000001u);
	CHECK_EQ(verticalMixers(0b000001, 0, false), 0b000001u);
	CHECK_EQ(verticalMixers(0b000001, 7, false), 0b000001u);
	/* idempotent */
	const uint32_t once = verticalMixers(0b010001, 6, false);
	CHECK_EQ(verticalMixers(once, 6, false), once);
}

TEST("audio: exclusions set, prune and JSON")
{
	AudioExclusions e;
	CHECK(!e.excluded("a"));
	CHECK(e.setExcluded("a", true));
	CHECK(!e.setExcluded("a", true));
	CHECK(e.setExcluded("b", true));
	CHECK(!e.setExcluded("", true));
	CHECK(e.excluded("a"));
	CHECK_EQ(e.size(), 2u);
	CHECK(e.setExcluded("b", false));
	CHECK(!e.setExcluded("b", false));
	CHECK(!e.excluded("b"));
	e.setExcluded("c", true);
	CHECK_EQ(e.prune({"a", "x"}), 1u);
	CHECK(e.excluded("a"));
	CHECK(!e.excluded("c"));
	const AudioExclusions back = AudioExclusions::fromJson(Json::parse(e.toJson().dump()));
	CHECK(back == e);
	const AudioExclusions junk = AudioExclusions::fromJson(Json::parse("[1, null, \"z\", \"\"]"));
	CHECK_EQ(junk.size(), 1u);
	CHECK(junk.excluded("z"));
}

TEST("audio: rows: new sources are sent, globals first, sorted by name, one per uuid")
{
	AudioExclusions e;
	e.setExcluded("music", true);
	std::vector<AudioSourceInfo> s = {
		{"music", "Music", 0b000001, false},         {"mic", "Mic/Aux", 0b000001, true},
		{"game", "game capture", 0b000010, false},   {"browser", "Browser", 0b000001, false},
		{"music", "Music (again)", 0b000001, false}, {"", "no uuid", 1, false},
		{"desk", "Desktop Audio", 0b000001, true},
	};
	const auto rows = audioRows(s, e);
	CHECK_EQ(rows.size(), 5u);
	CHECK_EQ(rows[0].name, std::string("Desktop Audio"));
	CHECK_EQ(rows[1].name, std::string("Mic/Aux"));
	CHECK_EQ(rows[2].name, std::string("Browser"));
	CHECK_EQ(rows[3].name, std::string("game capture"));
	CHECK_EQ(rows[4].name, std::string("Music"));
	CHECK(rows[2].sent);
	CHECK(!rows[4].sent);
	CHECK(!rows[3].onMainMix);
	CHECK(rows[0].global && !rows[2].global);
}

TEST("audio: settings keep the track and the exclusions")
{
	Settings s;
	s.audio.track = 5;
	s.audio.exclusions.setExcluded("{uuid-music}", true);
	const Settings back = loadSettings(saveSettings(s));
	CHECK_EQ(back.audio.track, 5);
	CHECK(back.audio.exclusions == s.audio.exclusions);
	std::vector<std::string> w;
	const Settings bad = loadSettings("{\"audio\":{\"track\":9}}", &w);
	CHECK_EQ(bad.audio.track, 0);
	CHECK(!w.empty());
}

TEST("audio: settings 1 move track 1 encodings to the vertical mix")
{
	const char *v1 = "{\"version\":1,\"destinations\":[{\"id\":\"a\",\"name\":\"A\",\"platform\":\"tiktok\","
			 "\"encoding\":{\"audio\":{\"bitrate\":160,\"track\":1}}},{\"id\":\"b\",\"name\":\"B\","
			 "\"platform\":\"youtube\",\"encoding\":{\"audio\":{\"bitrate\":160,\"track\":3}}}],"
			 "\"recording\":{\"encoding\":{\"audio\":{\"track\":1}}}}";
	const Settings s = loadSettings(v1);
	CHECK_EQ(s.destinations.size(), 2u);
	CHECK_EQ(s.destinations[0].encoding.audio.track, 0);
	CHECK_EQ(s.destinations[1].encoding.audio.track, 3);
	CHECK_EQ(s.recording.encoding.audio.track, 0);
	/* Version 2 keeps an explicit track 1. */
	const Settings v2 = loadSettings("{\"version\":2,\"recording\":{\"encoding\":{\"audio\":{\"track\":1}}}}");
	CHECK_EQ(v2.recording.encoding.audio.track, 1);
	CHECK_EQ(Settings().recording.encoding.audio.track, 0);
}
