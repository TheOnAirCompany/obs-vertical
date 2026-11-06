#include "dock-model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vertical::core {

/* ------------------------------------------------------------------ layout mode */

const char *layoutChoiceId(LayoutChoice c)
{
	switch (c) {
	case LayoutChoice::Compact:
		return "compact";
	case LayoutChoice::Studio:
		return "studio";
	default:
		return "auto";
	}
}

LayoutChoice layoutChoiceFromId(const std::string &id)
{
	if (id == "compact")
		return LayoutChoice::Compact;
	if (id == "studio")
		return LayoutChoice::Studio;
	return LayoutChoice::Automatic;
}

DockMode chooseDockMode(int width, LayoutChoice choice, DockMode current)
{
	if (choice == LayoutChoice::Compact)
		return DockMode::Compact;
	if (choice == LayoutChoice::Studio)
		return DockMode::Studio;
	if (current == DockMode::Studio)
		return width < kStudioMinWidth - kModeHysteresis ? DockMode::Compact : DockMode::Studio;
	return width >= kStudioMinWidth ? DockMode::Studio : DockMode::Compact;
}

/* ------------------------------------------------------------------ status lines */

std::string fillTemplate(const std::string &tpl, const std::string &a1, const std::string &a2)
{
	std::string out;
	out.reserve(tpl.size() + a1.size() + a2.size());
	for (size_t i = 0; i < tpl.size(); i++) {
		if (tpl[i] == '%' && i + 1 < tpl.size() && (tpl[i + 1] == '1' || tpl[i + 1] == '2')) {
			out += tpl[i + 1] == '1' ? a1 : a2;
			i++;
			continue;
		}
		out += tpl[i];
	}
	return out;
}

std::string formatClock(uint64_t seconds)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu", (unsigned long long)(seconds / 3600),
		      (unsigned long long)((seconds / 60) % 60), (unsigned long long)(seconds % 60));
	return buf;
}

std::string formatMbps(double kbps, char decimal)
{
	if (!(kbps > 0.0))
		kbps = 0.0;
	/* One decimal, rounded half up on the tenths of Mbps. */
	const long long tenths = (long long)std::floor(kbps / 100.0 + 0.5);
	std::string out = std::to_string(tenths / 10);
	out += decimal;
	out += (char)('0' + (int)(tenths % 10));
	return out;
}

double kbpsBetween(uint64_t bytesBefore, uint64_t nsBefore, uint64_t bytesNow, uint64_t nsNow)
{
	if (!nsBefore || nsNow <= nsBefore || bytesNow < bytesBefore)
		return -1.0;
	const double seconds = (double)(nsNow - nsBefore) / 1e9;
	return (double)(bytesNow - bytesBefore) * 8.0 / 1000.0 / seconds;
}

uint64_t elapsedSeconds(uint64_t startedNs, uint64_t nowNs)
{
	if (!startedNs || nowNs <= startedNs)
		return 0;
	return (nowNs - startedNs) / 1000000000ull;
}

StatusView describeStatus(const StatusInput &in, const StatusTexts &texts)
{
	StatusView v;
	switch (in.phase) {
	case Phase::Connecting:
		v.tone = Tone::Busy;
		v.running = true;
		v.text = texts.connecting;
		break;
	case Phase::Live: {
		v.tone = Tone::Live;
		v.running = true;
		v.text = texts.live + " · " + formatClock(in.elapsedSec);
		if (in.kbps >= 0.0)
			v.text += " · " + fillTemplate(texts.rate, formatMbps(in.kbps, texts.decimal));
		if (in.dropped > 0)
			v.text += " · " + fillTemplate(texts.dropped, std::to_string(in.dropped));
		break;
	}
	case Phase::Reconnecting:
		v.tone = Tone::Busy;
		v.running = true;
		v.text = in.attempt > 0 && in.maxAttempts > 0
				 ? fillTemplate(texts.reconnecting, std::to_string(in.attempt),
						std::to_string(in.maxAttempts))
				 : texts.reconnectingNoCount;
		break;
	case Phase::Stopping:
		v.tone = Tone::Busy;
		v.running = true;
		v.text = texts.stopping;
		break;
	case Phase::Error:
		v.tone = Tone::Error;
		v.ring = true;
		v.text = in.error.empty() ? texts.errorNoDetail : fillTemplate(texts.error, in.error);
		break;
	default:
		v.text = texts.stopped;
		break;
	}
	return v;
}

int liveCount(const std::vector<Phase> &phases)
{
	return (int)std::count(phases.begin(), phases.end(), Phase::Live);
}

/* ------------------------------------------------------------------ header */

std::string canvasChip(int width, int height, uint32_t fpsNum, uint32_t fpsDen)
{
	std::string out = std::to_string(width) + " × " + std::to_string(height);
	if (!fpsNum || !fpsDen)
		return out;
	std::string fps;
	if (fpsNum % fpsDen == 0) {
		fps = std::to_string(fpsNum / fpsDen);
	} else {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.2f", (double)fpsNum / (double)fpsDen);
		fps = buf;
	}
	return out + " · " + fps + " FPS";
}

std::string aspectTag(int width, int height)
{
	if (width <= 0 || height <= 0)
		return std::string();
	int a = width, b = height;
	while (b) {
		const int t = a % b;
		a = b;
		b = t;
	}
	const int w = width / a, h = height / a;
	/* Near-9:16 sizes that do not reduce exactly (e.g. 1080 x 1918) still read 9:16. */
	if (w > 50 || h > 50) {
		const double r = (double)width / (double)height;
		if (std::fabs(r - 9.0 / 16.0) < 0.01)
			return "9:16";
		if (std::fabs(r - 4.0 / 5.0) < 0.01)
			return "4:5";
		if (std::fabs(r - 1.0) < 0.01)
			return "1:1";
	}
	return std::to_string(w) + ":" + std::to_string(h);
}

/* ------------------------------------------------------------------ link menu */

std::vector<LinkMenuEntry> linkMenuEntries(const SceneLinks &links, const std::vector<std::string> &mainScenes,
					   const std::string &verticalScene)
{
	std::vector<LinkMenuEntry> out;
	out.reserve(mainScenes.size());
	for (const std::string &m : mainScenes) {
		LinkMenuEntry e;
		e.mainScene = m;
		const auto v = links.verticalFor(m);
		if (v) {
			if (*v == verticalScene)
				e.checked = true;
			else
				e.linkedTo = *v;
		}
		out.push_back(e);
	}
	return out;
}

LinkAction linkActionFor(const LinkMenuEntry &entry, bool checked)
{
	if (checked)
		return entry.checked ? LinkAction::None : LinkAction::Link;
	return entry.checked ? LinkAction::Unlink : LinkAction::None;
}

/* ------------------------------------------------------------------ platform tiles */

TileStyle tileStyle(Platform p)
{
	TileStyle s;
	switch (p) {
	case Platform::TikTok:
		s.colors[0] = 0x111111;
		s.foreground = 0x25F4EE;
		s.glyph = TileGlyph::Note;
		break;
	case Platform::YouTube:
		s.colors[0] = 0xE62117;
		s.glyph = TileGlyph::Play;
		break;
	case Platform::Instagram:
		s.colors[0] = 0xF58529;
		s.colors[1] = 0xDD2A7B;
		s.colors[2] = 0x8134AF;
		s.stops = 3;
		s.glyph = TileGlyph::Camera;
		break;
	case Platform::Facebook:
		s.colors[0] = 0x1877F2;
		s.glyph = TileGlyph::LetterF;
		break;
	case Platform::CustomSrt:
		s.glyph = TileGlyph::Srt;
		s.themed = true;
		break;
	default:
		s.glyph = TileGlyph::Server;
		s.themed = true;
		break;
	}
	return s;
}

Platform tilePlatform(const Destination &d)
{
	const std::optional<Platform> fromServer = platformFromServer(d.server);
	if (!fromServer)
		return d.platform;
	if (*fromServer == Platform::CustomSrt)
		return d.platform == Platform::CustomRtmp ? Platform::CustomSrt : d.platform;
	return *fromServer;
}

/* ------------------------------------------------------------------ virtual camera */

VcamState vcamState(bool supported, bool extensionInstalled, bool installRequested, bool running)
{
	if (!supported)
		return VcamState::Unavailable;
	if (running)
		return VcamState::Running;
	if (extensionInstalled)
		return VcamState::Activated;
	return installRequested ? VcamState::WaitingApproval : VcamState::NotInstalled;
}

} // namespace vertical::core
