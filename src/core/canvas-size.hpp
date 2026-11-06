#pragma once

/* Size of the vertical canvas: presets, validation, output scaling. Pure C++17. */

#include <string>
#include <vector>

namespace vertical::core {

struct Size {
	int width = 0;
	int height = 0;

	bool operator==(const Size &o) const { return width == o.width && height == o.height; }
	bool operator!=(const Size &o) const { return !(*this == o); }
};

struct CanvasPreset {
	const char *id;       /* "1080x1920" */
	Size size;
	const char *labelKey; /* locale key */
};

/* 1080x1920 first (the default), then 720x1280, 1440x2560, 2160x3840,
 * 1080x1350 (4:5) and 1080x1080 (1:1). */
const std::vector<CanvasPreset> &canvasPresets();
Size defaultCanvasSize();
const CanvasPreset *findPreset(const Size &s);

constexpr int kMinCanvasSide = 128;
constexpr int kMaxCanvasSide = 8192;

enum class SizeIssue { None, TooSmall, TooLarge, OddDimension, NotPortrait };

/* NotPortrait is a warning (square and 4:5 are allowed, landscape is not the
 * point of the plugin); the other issues are errors. */
SizeIssue checkCanvasSize(const Size &s);
bool isSizeError(SizeIssue i);

/* Output size with the base aspect ratio and the given output height, rounded
 * to even numbers (encoders want even sizes). outputHeight <= 0 or >= base
 * height returns the base size. */
Size scaledOutput(const Size &base, int outputHeight);

/* "1080x1920" (also accepts "1080 x 1920" and the × sign); {0,0} when invalid. */
Size parseSize(const std::string &text);
std::string formatSize(const Size &s);

} // namespace vertical::core
