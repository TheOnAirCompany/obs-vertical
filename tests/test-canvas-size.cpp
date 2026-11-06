#include "test.hpp"

#include "canvas-size.hpp"

using namespace vertical::core;

TEST("canvas: presets, default first")
{
	CHECK(defaultCanvasSize() == (Size{1080, 1920}));
	CHECK(canvasPresets().size() >= 4);
	for (const CanvasPreset &p : canvasPresets()) {
		CHECK(!isSizeError(checkCanvasSize(p.size)));
		CHECK_EQ(formatSize(p.size), std::string(p.id));
		CHECK(findPreset(p.size) == &p);
	}
	CHECK(findPreset({1000, 1000}) == nullptr);
}

TEST("canvas: size checks")
{
	CHECK(checkCanvasSize({1080, 1920}) == SizeIssue::None);
	CHECK(checkCanvasSize({64, 1920}) == SizeIssue::TooSmall);
	CHECK(checkCanvasSize({1080, 9000}) == SizeIssue::TooLarge);
	CHECK(checkCanvasSize({1081, 1920}) == SizeIssue::OddDimension);
	CHECK(checkCanvasSize({1920, 1080}) == SizeIssue::NotPortrait);
	CHECK(!isSizeError(SizeIssue::NotPortrait));
	CHECK(isSizeError(SizeIssue::OddDimension));
	CHECK(checkCanvasSize({1080, 1080}) == SizeIssue::None);
}

TEST("canvas: scaled output keeps the ratio and even sizes")
{
	CHECK(scaledOutput({1080, 1920}, 1280) == (Size{720, 1280}));
	CHECK(scaledOutput({1080, 1920}, 0) == (Size{1080, 1920}));
	CHECK(scaledOutput({1080, 1920}, 1920) == (Size{1080, 1920}));
	CHECK(scaledOutput({1080, 1920}, 4000) == (Size{1080, 1920}));
	const Size s = scaledOutput({1080, 1920}, 853);
	CHECK(s.width % 2 == 0);
	CHECK(s.height % 2 == 0);
	CHECK_EQ(s.height, 852);
	CHECK_EQ(s.width, 478);
	CHECK(scaledOutput({1080, 1350}, 1000) == (Size{800, 1000}));
}

TEST("canvas: parse sizes")
{
	CHECK(parseSize("1080x1920") == (Size{1080, 1920}));
	CHECK(parseSize(" 720 X 1280 ") == (Size{720, 1280}));
	CHECK(parseSize("1080\xC3\x97" "1920") == (Size{1080, 1920}));
	CHECK(parseSize("1080x") == Size{});
	CHECK(parseSize("x1920") == Size{});
	CHECK(parseSize("1080x1920p") == Size{});
	CHECK(parseSize("1080-1920") == Size{});
	CHECK(parseSize("") == Size{});
}
