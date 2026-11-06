#pragma once

/* Design decisions of the Vertical dock (0.2), one switch per decision of the
 * design review (the id in each comment is the review page block). Each switch
 * holds the recommended option; changing a constant here changes the dock. */

namespace vertical::ui::decisions {

/* modes: how the dock picks Studio (two columns) or Compact (one column). */
enum class Modes {
	Automatic,            /* by width only */
	Manual,               /* chosen in the ⋯ menu only (Studio by default) */
	AutomaticOverridable, /* by width, "Always Compact" / "Always Studio" in ⋯ */
};
inline constexpr Modes kModes = Modes::AutomaticOverridable;

/* entete: first line of the dock. */
enum class Header {
	CanvasLine, /* "1080 × 1920 · 30 FPS", linked scenes switch, ⋯ */
	None,       /* no header line: the switch and ⋯ move to the scenes section */
};
inline constexpr Header kHeader = Header::CanvasLine;

/* apercu-cadre: frame around the preview while a destination is live. */
enum class PreviewFrame { Red, Violet, None };
inline constexpr PreviewFrame kPreviewFrame = PreviewFrame::Red;

/* scene-programme: how the scene on air shows in the list. */
enum class ProgramRow {
	Blue,    /* filled blue row with ▶ */
	Red,     /* red tint, red bar on the left and a PROGRAM badge */
	Outline, /* red outline, no badge */
};
inline constexpr ProgramRow kProgramRow = ProgramRow::Red;

/* liaison: link a vertical scene to 16:9 scenes from a link icon on each row
 * (a menu with checkmarks). The other options of the review (a separate
 * "Linked scenes" section) are not built. */
inline constexpr bool kLinkIconOnRows = true;

/* destinations: platform tiles in the platform colours, or monochrome. */
enum class Tiles { Colored, Mono };
inline constexpr Tiles kTiles = Tiles::Colored;

/* enregistrement: tabs in Studio and three lines in Compact, or lines everywhere. */
enum class Recording { TabsInStudio, LinesEverywhere };
inline constexpr Recording kRecording = Recording::TabsInStudio;

/* camera-virtuelle: "Beta" badge until verified on a real Mac, or a plain note. */
enum class VirtualCamBeta { Badge, Note };
inline constexpr VirtualCamBeta kVirtualCamBeta = VirtualCamBeta::Badge;

/* pied: status bar at the bottom. */
enum class Footer { OneLine, TwoLines, HiddenInCompact };
inline constexpr Footer kFooter = Footer::OneLine;

/* couleurs: OBS theme palette (with fixed status colours) or the fixed palette of the mock. */
enum class Colors { ObsTheme, MockPalette };
inline constexpr Colors kColors = Colors::ObsTheme;

/* tailles: maximum height of the preview. */
enum class PreviewSize { Max360, Max480, ColumnWidth };
inline constexpr PreviewSize kPreviewSize = PreviewSize::Max360;

} // namespace vertical::ui::decisions
