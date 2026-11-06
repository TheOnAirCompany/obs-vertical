#include "test.hpp"

#include "dock-model.hpp"

using namespace vertical::core;

TEST("dock: layout mode by width, with hysteresis")
{
	CHECK(chooseDockMode(300, LayoutChoice::Automatic, DockMode::Compact) == DockMode::Compact);
	CHECK(chooseDockMode(kStudioMinWidth - 1, LayoutChoice::Automatic, DockMode::Compact) == DockMode::Compact);
	CHECK(chooseDockMode(kStudioMinWidth, LayoutChoice::Automatic, DockMode::Compact) == DockMode::Studio);
	CHECK(chooseDockMode(640, LayoutChoice::Automatic, DockMode::Compact) == DockMode::Studio);
	/* Once in Studio it stays a little below the threshold. */
	CHECK(chooseDockMode(kStudioMinWidth - 1, LayoutChoice::Automatic, DockMode::Studio) == DockMode::Studio);
	CHECK(chooseDockMode(kStudioMinWidth - kModeHysteresis, LayoutChoice::Automatic, DockMode::Studio) ==
	      DockMode::Studio);
	CHECK(chooseDockMode(kStudioMinWidth - kModeHysteresis - 1, LayoutChoice::Automatic, DockMode::Studio) ==
	      DockMode::Compact);
}

TEST("dock: forced layout ignores the width")
{
	CHECK(chooseDockMode(1200, LayoutChoice::Compact, DockMode::Studio) == DockMode::Compact);
	CHECK(chooseDockMode(200, LayoutChoice::Studio, DockMode::Compact) == DockMode::Studio);
	CHECK(layoutChoiceFromId(layoutChoiceId(LayoutChoice::Compact)) == LayoutChoice::Compact);
	CHECK(layoutChoiceFromId(layoutChoiceId(LayoutChoice::Studio)) == LayoutChoice::Studio);
	CHECK(layoutChoiceFromId("auto") == LayoutChoice::Automatic);
	CHECK(layoutChoiceFromId("garbage") == LayoutChoice::Automatic);
}

TEST("dock: clock and rate formatting")
{
	CHECK_EQ(formatClock(0), std::string("00:00:00"));
	CHECK_EQ(formatClock(754), std::string("00:12:34"));
	CHECK_EQ(formatClock(36000 + 61), std::string("10:01:01"));
	CHECK_EQ(formatClock(100 * 3600), std::string("100:00:00"));
	CHECK_EQ(formatMbps(6000), std::string("6.0"));
	CHECK_EQ(formatMbps(6049), std::string("6.0"));
	CHECK_EQ(formatMbps(6050), std::string("6.1"));
	CHECK_EQ(formatMbps(850, ','), std::string("0,9"));
	CHECK_EQ(formatMbps(-3), std::string("0.0"));
	CHECK_EQ(formatMbps(12345), std::string("12.3"));
	CHECK_EQ(fillTemplate("%1 of %2 (%1)", "a", "b"), std::string("a of b (a)"));
	CHECK_EQ(fillTemplate("100% sure", "x"), std::string("100% sure"));
}

TEST("dock: bitrate from byte counters")
{
	/* 750 000 bytes in one second = 6000 kbps. */
	CHECK_NEAR(kbpsBetween(1000, 1000000000ull, 751000, 2000000000ull), 6000.0, 1e-6);
	/* Half a second. */
	CHECK_NEAR(kbpsBetween(0, 1000000000ull, 375000, 1500000000ull), 6000.0, 1e-6);
	CHECK(kbpsBetween(0, 0, 1000, 1000000000ull) < 0);                            /* first sample */
	CHECK(kbpsBetween(0, 1000000000ull, 1000, 1000000000ull) < 0);                /* clock did not move */
	CHECK(kbpsBetween(5000, 1000000000ull, 1000, 2000000000ull) < 0);             /* counter reset */
	CHECK_NEAR(kbpsBetween(1000, 1000000000ull, 1000, 2000000000ull), 0.0, 1e-9); /* stalled */
	CHECK_EQ(elapsedSeconds(0, 5000000000ull), 0u);
	CHECK_EQ(elapsedSeconds(1000000000ull, 755500000000ull), 754u);
	CHECK_EQ(elapsedSeconds(9000000000ull, 1000000000ull), 0u);
}

TEST("dock: status lines from the output states")
{
	StatusTexts t;
	StatusInput in;
	StatusView v = describeStatus(in, t);
	CHECK(v.tone == Tone::Idle && !v.running && !v.ring);
	CHECK_EQ(v.text, std::string("Stopped"));

	in.phase = Phase::Connecting;
	v = describeStatus(in, t);
	CHECK(v.tone == Tone::Busy && v.running);
	CHECK_EQ(v.text, std::string("Connecting…"));

	in.phase = Phase::Live;
	in.elapsedSec = 754;
	in.kbps = 6000;
	v = describeStatus(in, t);
	CHECK(v.tone == Tone::Live && v.running);
	CHECK_EQ(v.text, std::string("Live · 00:12:34 · 6.0 Mbps"));
	in.dropped = 3;
	CHECK_EQ(describeStatus(in, t).text, std::string("Live · 00:12:34 · 6.0 Mbps · 3 dropped"));
	in.dropped = 0;
	in.kbps = -1;
	CHECK_EQ(describeStatus(in, t).text, std::string("Live · 00:12:34"));

	in.phase = Phase::Reconnecting;
	in.attempt = 2;
	in.maxAttempts = 25;
	v = describeStatus(in, t);
	CHECK(v.tone == Tone::Busy && v.running);
	CHECK_EQ(v.text, std::string("Reconnecting 2/25…"));
	in.attempt = 0;
	CHECK_EQ(describeStatus(in, t).text, std::string("Reconnecting…"));

	in.phase = Phase::Error;
	in.error = "stream key refused";
	v = describeStatus(in, t);
	CHECK(v.tone == Tone::Error && v.ring && !v.running);
	CHECK_EQ(v.text, std::string("Error: stream key refused"));
	in.error.clear();
	CHECK_EQ(describeStatus(in, t).text, std::string("Error"));

	in.phase = Phase::Stopping;
	CHECK(describeStatus(in, t).running);
}

TEST("dock: translated status texts")
{
	StatusTexts fr;
	fr.live = "En direct";
	fr.rate = "%1 Mb/s";
	fr.decimal = ',';
	StatusInput in;
	in.phase = Phase::Live;
	in.elapsedSec = 754;
	in.kbps = 6000;
	CHECK_EQ(describeStatus(in, fr).text, std::string("En direct · 00:12:34 · 6,0 Mb/s"));
}

TEST("dock: live count")
{
	CHECK_EQ(liveCount({}), 0);
	CHECK_EQ(liveCount({Phase::Live, Phase::Stopped, Phase::Live, Phase::Reconnecting, Phase::Error}), 2);
}

TEST("dock: canvas chip and aspect tag")
{
	CHECK_EQ(canvasChip(1080, 1920, 30, 1), std::string("1080 × 1920 · 30 FPS"));
	CHECK_EQ(canvasChip(1080, 1920, 60000, 1001), std::string("1080 × 1920 · 59.94 FPS"));
	CHECK_EQ(canvasChip(720, 1280, 0, 0), std::string("720 × 1280"));
	CHECK_EQ(aspectTag(1080, 1920), std::string("9:16"));
	CHECK_EQ(aspectTag(720, 1280), std::string("9:16"));
	CHECK_EQ(aspectTag(1080, 1350), std::string("4:5"));
	CHECK_EQ(aspectTag(1080, 1080), std::string("1:1"));
	CHECK_EQ(aspectTag(1080, 1918), std::string("9:16"));
	CHECK_EQ(aspectTag(1000, 3000), std::string("1:3"));
	CHECK_EQ(aspectTag(0, 1920), std::string());
}

TEST("dock: link menu lists main scenes with checkmarks")
{
	SceneLinks l;
	l.link("Plateau", "Talking");
	l.link("Interview", "Talking");
	l.link("Gameplay", "V Game");
	const auto e = linkMenuEntries(l, {"Plateau", "Interview", "Gameplay", "Pause"}, "Talking");
	CHECK_EQ(e.size(), 4u);
	CHECK(e[0].checked && e[0].linkedTo.empty());
	CHECK(e[1].checked);
	CHECK(!e[2].checked);
	CHECK_EQ(e[2].linkedTo, std::string("V Game"));
	CHECK(!e[3].checked && e[3].linkedTo.empty());
	CHECK_EQ(e[0].mainScene, std::string("Plateau"));
	CHECK(linkMenuEntries(l, {}, "Talking").empty());
}

TEST("dock: link menu toggles")
{
	SceneLinks l;
	l.link("Gameplay", "V Game");
	auto e = linkMenuEntries(l, {"Plateau", "Gameplay"}, "Talking");
	CHECK(linkActionFor(e[0], true) == LinkAction::Link);
	CHECK(linkActionFor(e[0], false) == LinkAction::None);
	/* Checking a main scene that follows another vertical scene moves the link. */
	CHECK(linkActionFor(e[1], true) == LinkAction::Link);
	/* Unchecking it from here does not touch the other vertical scene's link. */
	CHECK(linkActionFor(e[1], false) == LinkAction::None);
	l.link("Plateau", "Talking");
	e = linkMenuEntries(l, {"Plateau", "Gameplay"}, "Talking");
	CHECK(linkActionFor(e[0], false) == LinkAction::Unlink);
	CHECK(linkActionFor(e[0], true) == LinkAction::None);
}

TEST("dock: platform tiles")
{
	CHECK(tileStyle(Platform::TikTok).glyph == TileGlyph::Note);
	CHECK_EQ(tileStyle(Platform::TikTok).foreground, 0x25F4EEu);
	CHECK_EQ(tileStyle(Platform::YouTube).colors[0], 0xE62117u);
	CHECK_EQ(tileStyle(Platform::Instagram).stops, 3);
	CHECK(tileStyle(Platform::Facebook).glyph == TileGlyph::LetterF);
	CHECK(tileStyle(Platform::CustomRtmp).themed);
	CHECK(tileStyle(Platform::CustomSrt).glyph == TileGlyph::Srt);
	for (Platform p : allPlatforms())
		CHECK(tileStyle(p).stops >= 1 && tileStyle(p).stops <= 3);
}

TEST("dock: platform from the server URL")
{
	CHECK(platformFromServer("rtmp://push.tiktokcdn.com/game/") == Platform::TikTok);
	CHECK(platformFromServer("rtmp://push-rtmp-l11-va01.tiktokcdn.com/game/") == Platform::TikTok);
	CHECK(platformFromServer("rtmps://push-rtmp-l3-fra01.tiktokcdn-eu.com:443/game") == Platform::TikTok);
	CHECK(platformFromServer("rtmp://a.rtmp.youtube.com/live2") == Platform::YouTube);
	CHECK(platformFromServer("rtmps://a.rtmps.youtube.com:443/live2") == Platform::YouTube);
	CHECK(platformFromServer("rtmps://edgetee-upload-cdg4-2.xx.fbcdn.net:443/rtmp/") == Platform::Instagram);
	CHECK(platformFromServer("rtmps://edgetee-upload-mxp1-1.instagram.com:443/rtmp/") == Platform::Instagram);
	CHECK(platformFromServer("rtmps://live-upload.instagram.com:443/rtmp/") == Platform::Instagram);
	CHECK(platformFromServer("rtmps://live-api-s.facebook.com:443/rtmp/") == Platform::Facebook);
	CHECK(platformFromServer("  RTMPS://Live-API-S.Facebook.com:443/rtmp/  ") == Platform::Facebook);
	CHECK(platformFromServer("srt://192.168.1.20:9000?streamid=live") == Platform::CustomSrt);
	CHECK(platformFromServer("push.tiktokcdn.com/game") == Platform::TikTok); /* no scheme */
	CHECK(!platformFromServer("rtmp://live.example.com/app"));
	CHECK(!platformFromServer("rtmp://127.0.0.1:1935/live"));
	CHECK(!platformFromServer("rtmp://notyoutube.example.com/live")); /* a label, not the domain */
	CHECK(!platformFromServer("rtmp://tiktok.example.com/live"));
	CHECK(!platformFromServer(""));
}

TEST("dock: tile of a destination, by platform field and by server URL")
{
	/* By the platform field: blank servers (TikTok and Instagram have no default one). */
	for (Platform p : allPlatforms())
		CHECK(tilePlatform(makeDestination(p, "id", "name")) == p);
	CHECK(tileStyle(tilePlatform(makeDestination(Platform::TikTok, "id", "TikTok"))).glyph == TileGlyph::Note);
	CHECK(tileStyle(tilePlatform(makeDestination(Platform::Instagram, "id", "Insta"))).glyph == TileGlyph::Camera);

	/* By the server URL: a TikTok server never shows the Instagram tile. */
	Destination d = makeDestination(Platform::Instagram, "id", "TikTok");
	d.server = "rtmp://push.tiktokcdn.com/game/";
	CHECK(tilePlatform(d) == Platform::TikTok);
	CHECK(tileStyle(tilePlatform(d)).glyph == TileGlyph::Note);
	d.platform = Platform::CustomRtmp;
	CHECK(tilePlatform(d) == Platform::TikTok);
	d.server = "rtmps://live-api-s.facebook.com:443/rtmp/";
	CHECK(tilePlatform(d) == Platform::Facebook);
	d.server = "rtmps://edgetee-upload-cdg4-2.xx.fbcdn.net:443/rtmp/";
	CHECK(tilePlatform(d) == Platform::Instagram);
	d.server = "rtmp://a.rtmp.youtube.com/live2";
	CHECK(tilePlatform(d) == Platform::YouTube);

	/* Custom servers keep the platform field; srt:// turns custom RTMP into SRT only. */
	d.server = "rtmp://live.example.com/app";
	CHECK(tilePlatform(d) == Platform::CustomRtmp);
	d.platform = Platform::TikTok;
	CHECK(tilePlatform(d) == Platform::TikTok);
	d.server = "srt://203.0.113.5:9000";
	CHECK(tilePlatform(d) == Platform::TikTok);
	d.platform = Platform::CustomRtmp;
	CHECK(tilePlatform(d) == Platform::CustomSrt);
	d.platform = Platform::CustomSrt;
	CHECK(tilePlatform(d) == Platform::CustomSrt);
	CHECK(tileStyle(tilePlatform(d)).glyph == TileGlyph::Srt);
}

TEST("dock: virtual camera state")
{
	CHECK(vcamState(false, true, true, true) == VcamState::Unavailable);
	CHECK(vcamState(true, false, false, false) == VcamState::NotInstalled);
	CHECK(vcamState(true, false, true, false) == VcamState::WaitingApproval);
	CHECK(vcamState(true, true, true, false) == VcamState::Activated);
	CHECK(vcamState(true, true, false, false) == VcamState::Activated);
	CHECK(vcamState(true, true, false, true) == VcamState::Running);
}
