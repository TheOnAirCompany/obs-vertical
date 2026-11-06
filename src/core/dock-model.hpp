#pragma once

/* Logic behind the Vertical dock (0.2): layout mode by width, status lines of
 * the outputs, bitrate, canvas chip, link menu, platform tiles, encoder chip and
 * virtual camera state. Pure C++17 (no OBS, no Qt): unit-tested in
 * tests/test-dock-model.cpp. The texts come translated from the caller. */

#include "destination.hpp"
#include "scene-links.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vertical::core {

/* ------------------------------------------------------------------ layout mode */

enum class DockMode { Compact, Studio };
/* Decision "modes": automatic by width, can be forced in the ⋯ menu. */
enum class LayoutChoice { Automatic, Compact, Studio };

/* From the review page: "Compact below ~520 px, Studio above". */
constexpr int kStudioMinWidth = 520;
/* provisional: Studio stays until the dock is this much narrower, so a width
 * near the threshold does not flip the layout back and forth. */
constexpr int kModeHysteresis = 16;

const char *layoutChoiceId(LayoutChoice c); /* "auto", "compact", "studio" */
LayoutChoice layoutChoiceFromId(const std::string &id);

/* Mode for a dock `width` px wide, given the user's choice and the current mode. */
DockMode chooseDockMode(int width, LayoutChoice choice, DockMode current);

/* ------------------------------------------------------------------ status lines */

enum class Phase { Stopped, Connecting, Live, Reconnecting, Stopping, Error };
/* Semantic colour of a status dot (fixed colours, see ui/dock-style.hpp). */
enum class Tone { Idle, Busy, Live, Error, Ok };

/* Translated templates; %1, %2 are replaced. */
struct StatusTexts {
	std::string live = "Live";
	std::string connecting = "Connecting…";
	std::string reconnecting = "Reconnecting %1/%2…";
	std::string reconnectingNoCount = "Reconnecting…";
	std::string stopping = "Stopping…";
	std::string stopped = "Stopped";
	std::string error = "Error: %1";
	std::string errorNoDetail = "Error";
	std::string rate = "%1 Mbps";
	std::string dropped = "%1 dropped";
	char decimal = '.';
};

struct StatusInput {
	Phase phase = Phase::Stopped;
	uint64_t elapsedSec = 0;
	double kbps = -1.0; /* < 0: not measured yet */
	int dropped = 0;
	int attempt = 0; /* reconnect attempt, 1-based; 0 = unknown */
	int maxAttempts = 0;
	std::string error;
};

struct StatusView {
	Tone tone = Tone::Idle;
	bool ring = false;    /* hollow dot (error) */
	bool running = false; /* the row shows ■ (stop) rather than ▶ */
	std::string text;
};

/* "● Live · 00:12:34 · 6.0 Mbps", "Reconnecting 2/25…", "Error: <message>", ... */
StatusView describeStatus(const StatusInput &in, const StatusTexts &texts);

/* "00:12:34" (hours on two digits at least). */
std::string formatClock(uint64_t seconds);
/* kbps -> Mbps with one decimal: 6000 -> "6.0" (or "6,0"). */
std::string formatMbps(double kbps, char decimal = '.');
/* Replaces %1 / %2 in a template. */
std::string fillTemplate(const std::string &tpl, const std::string &a1, const std::string &a2 = std::string());

/* Bitrate in kbps between two samples of obs_output_get_total_bytes, or a
 * negative value when it cannot be computed (first sample, clock not moving,
 * counter reset by a reconnect). */
double kbpsBetween(uint64_t bytesBefore, uint64_t nsBefore, uint64_t bytesNow, uint64_t nsNow);

/* Whole seconds since `startedNs` (0 when not started or the clock is behind). */
uint64_t elapsedSeconds(uint64_t startedNs, uint64_t nowNs);

/* Number of streams on air (Live only). */
int liveCount(const std::vector<Phase> &phases);

/* ------------------------------------------------------------------ header */

/* "1080 × 1920 · 30 FPS", "1080 × 1920 · 29.97 FPS". */
std::string canvasChip(int width, int height, uint32_t fpsNum, uint32_t fpsDen);
/* Aspect tag of the preview: "9:16", "4:5", "1:1", else the reduced ratio. */
std::string aspectTag(int width, int height);

/* ------------------------------------------------------------------ link menu */

struct LinkMenuEntry {
	std::string mainScene;
	bool checked = false; /* linked to this vertical scene */
	std::string linkedTo; /* another vertical scene it follows today, else empty */
};

/* One entry per main (16:9) scene, in the given order. */
std::vector<LinkMenuEntry> linkMenuEntries(const SceneLinks &links, const std::vector<std::string> &mainScenes,
					   const std::string &verticalScene);

enum class LinkAction { None, Link, Unlink };
/* What toggling an entry to `checked` does. Unchecking a main scene linked to
 * another vertical scene does nothing. */
LinkAction linkActionFor(const LinkMenuEntry &entry, bool checked);

/* ------------------------------------------------------------------ platform tiles */

enum class TileGlyph { Note, Play, Camera, LetterF, Server, Srt };

struct TileStyle {
	/* 0xRRGGBB. stops > 1: diagonal gradient (top left to bottom right). */
	uint32_t colors[3] = {0, 0, 0};
	int stops = 1;
	uint32_t foreground = 0xFFFFFF;
	TileGlyph glyph = TileGlyph::Server;
	bool themed = false; /* custom servers: colours from the OBS theme instead */
};
TileStyle tileStyle(Platform p);

/* ------------------------------------------------------------------ encoder chip */

/* Short family name of an encoder id: "VideoToolbox", "NVENC", "AMF", "Quick Sync",
 * "VA-API", "x264", ... or the id itself. */
std::string encoderFamily(const std::string &encoderId);

/* ------------------------------------------------------------------ virtual camera */

enum class VcamState { Unavailable, NotInstalled, WaitingApproval, Activated, Running };
/* supported: built with the virtual camera (macOS); installRequested: the user
 * asked the host app to install the extension and macOS has not approved yet. */
VcamState vcamState(bool supported, bool extensionInstalled, bool installRequested, bool running);

} // namespace vertical::core
