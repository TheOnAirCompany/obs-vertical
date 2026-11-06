#include "canvas-size.hpp"

#include <cctype>
#include <cmath>

namespace vertical::core {

const std::vector<CanvasPreset> &canvasPresets()
{
	static const std::vector<CanvasPreset> presets{
		{"1080x1920", {1080, 1920}, "Canvas.Preset.FullHD"},
		{"720x1280", {720, 1280}, "Canvas.Preset.HD"},
		{"1440x2560", {1440, 2560}, "Canvas.Preset.QHD"},
		{"2160x3840", {2160, 3840}, "Canvas.Preset.UHD"},
		{"1080x1350", {1080, 1350}, "Canvas.Preset.Portrait45"},
		{"1080x1080", {1080, 1080}, "Canvas.Preset.Square"},
	};
	return presets;
}

Size defaultCanvasSize()
{
	return canvasPresets().front().size;
}

const CanvasPreset *findPreset(const Size &s)
{
	for (const CanvasPreset &p : canvasPresets())
		if (p.size == s)
			return &p;
	return nullptr;
}

SizeIssue checkCanvasSize(const Size &s)
{
	if (s.width < kMinCanvasSide || s.height < kMinCanvasSide)
		return SizeIssue::TooSmall;
	if (s.width > kMaxCanvasSide || s.height > kMaxCanvasSide)
		return SizeIssue::TooLarge;
	if (s.width % 2 || s.height % 2)
		return SizeIssue::OddDimension;
	if (s.width > s.height)
		return SizeIssue::NotPortrait;
	return SizeIssue::None;
}

bool isSizeError(SizeIssue i)
{
	return i == SizeIssue::TooSmall || i == SizeIssue::TooLarge || i == SizeIssue::OddDimension;
}

Size scaledOutput(const Size &base, int outputHeight)
{
	if (outputHeight <= 0 || outputHeight >= base.height || base.height <= 0)
		return base;
	const double ratio = (double)base.width / (double)base.height;
	int h = outputHeight - (outputHeight % 2);
	int w = (int)std::lround(h * ratio);
	w -= w % 2;
	if (w < 2)
		w = 2;
	if (h < 2)
		h = 2;
	return {w, h};
}

Size parseSize(const std::string &text)
{
	/* Digits, separator (x, X or ×), digits; spaces allowed around the separator. */
	size_t i = 0;
	const auto skipSpaces = [&] {
		while (i < text.size() && text[i] == ' ')
			i++;
	};
	const auto readInt = [&](int &out) {
		const size_t start = i;
		long v = 0;
		while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) && i - start < 6)
			v = v * 10 + (text[i++] - '0');
		out = (int)v;
		return i > start;
	};
	Size s;
	skipSpaces();
	if (!readInt(s.width))
		return {};
	skipSpaces();
	if (i < text.size() && (text[i] == 'x' || text[i] == 'X'))
		i++;
	else if (text.compare(i, 2, "\xC3\x97") == 0)
		i += 2;
	else
		return {};
	skipSpaces();
	if (!readInt(s.height))
		return {};
	skipSpaces();
	if (i != text.size())
		return {};
	return s;
}

std::string formatSize(const Size &s)
{
	return std::to_string(s.width) + "x" + std::to_string(s.height);
}

} // namespace vertical::core
