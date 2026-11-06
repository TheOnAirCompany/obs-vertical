#pragma once

/* Visual tokens and small widgets of the Vertical dock. Values come from the
 * design review page (Studio and Compact mocks); the ones the page does not give
 * are marked "provisional". Surfaces follow the active OBS theme (QPalette);
 * status colours are fixed. */

#include "core/dock-model.hpp"

#include <QAbstractButton>
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QWidget>

namespace vertical::ui {

namespace tokens {

/* ---- status colours (fixed, from the review page) ---- */
constexpr QRgb kRed = 0xFFE5484D;   /* live, error, recording */
constexpr QRgb kGreen = 0xFF30A46C; /* ready, activated, hardware encoder */
constexpr QRgb kAmber = 0xFFE3A748; /* connecting, reconnecting, waiting, beta */
constexpr QRgb kIdle = 0xFF6B7280;  /* stopped */
constexpr QRgb kErrorText = 0xFFF08A8D;
constexpr QRgb kProgramTint = 0x29E5484D; /* rgba(229,72,77,.16) */
constexpr QRgb kProgramBlue = 0xFF2F5BD3; /* option "blue" of scene-programme */
constexpr QRgb kViolet = 0xFF8B5CF6;      /* option "violet" of apercu-cadre */
constexpr QRgb kStopFill = 0xFF4A2427;    /* ■ button of a running output */
constexpr QRgb kStopBorder = 0xFF6B2E33;
constexpr QRgb kBigStop = 0xFFD93A40; /* "Stop Recording" */
constexpr QRgb kRecHalo = 0x2EE5484D; /* rgba(229,72,77,.18) behind the record dot */
constexpr QRgb kBetaText = 0xFF1B1300;
constexpr QRgb kPreviewTagFill = 0x73000000; /* "9:16" tag: rgba(0,0,0,.45) */

/* ---- sizes (logical px, from the review page) ---- */
constexpr int kRowStudio = 32;
constexpr int kRowCompact = 28;
constexpr int kButton = 28;      /* ▶ / ■ and icon buttons */
constexpr int kSmallButton = 24; /* ⋯, gear, + */
constexpr int kTile = 28;
constexpr int kTileRadius = 7;
constexpr int kTileCompact = 22;
constexpr int kTileCompactRadius = 6;
constexpr int kDot = 7;
constexpr int kPadding = 10; /* inside a column */
constexpr int kGap = 10;     /* between sections */
constexpr int kStudioLeftColumn = 220;
constexpr int kPreviewMaxStudio = 360; /* tailles: apercu-360 */
constexpr int kPreviewMax480 = 480;    /* tailles: apercu-480 */
constexpr int kPreviewMaxCompact = 300;
constexpr int kPreviewRadius = 10;
constexpr int kPreviewFrame = 2;
constexpr int kListRadius = 6;
constexpr int kRowRadius = 4;
constexpr int kProgramBar = 3;
constexpr int kSwitchW = 28;
constexpr int kSwitchH = 16;
constexpr int kBigButton = 34; /* Stop Recording, timer */
constexpr int kRecDot = 34;
constexpr int kIcon = 14;
constexpr int kIconSmall = 12;
/* Text sizes relative to the theme font (12 px in the mock). */
constexpr qreal kSmallText = 11.0 / 12.0;
constexpr qreal kBadgeText = 9.5 / 12.0;

/* provisional: values the review page does not give. */
constexpr int kPreviewMinHeight = 120;     /* provisional */
constexpr int kCollapsedBar = 24;          /* provisional: "Show preview" bar when hidden */
constexpr int kDetachedWidth = 360;        /* provisional: detached preview window */
constexpr int kDetachedHeight = 640;       /* provisional */
constexpr int kScenesMinRows = 3;          /* provisional: scene list keeps 3 rows visible */
constexpr int kCompactDestinationsMax = 4; /* provisional: rows before the list scrolls */
constexpr int kStudioDestinationsMax = 6;  /* provisional */
constexpr qreal kLineMix = 0.14;           /* provisional: separator = window mixed with text */
constexpr qreal kRowMix = 0.06;            /* provisional: list row hover/raised */
constexpr qreal kRaisedMix = 0.10;         /* provisional: tabs, tiles of custom servers */
constexpr qreal kMutedMix = 0.42;          /* provisional: secondary text */

} // namespace tokens

/* Surfaces of the dock: the OBS theme palette, or the fixed mock palette. */
struct Surfaces {
	QColor panel;  /* dock background */
	QColor base;   /* lists, tabs bar */
	QColor row;    /* buttons */
	QColor raised; /* selected tab, custom tiles */
	QColor line;   /* separators, borders */
	QColor text;
	QColor muted;
	QColor accent; /* links, switch on, selection */
};
Surfaces surfaces(const QWidget *w);

QColor toneColor(core::Tone t);

/* Icons of the dock (drawn from SVG paths, tinted). Names: play, stop, more, link,
 * screen, plus, gear, chev, rec, replay, cam, phone, note, insta, window. */
QIcon icon(const char *name, const QColor &color, int size = tokens::kIcon);
QPixmap iconPixmap(const char *name, const QColor &color, int size, qreal dpr);
bool iconsAvailable();

/* Sets a style sheet only when it changes (re-polishing is not free). */
void setSheet(QWidget *w, const QString &sheet);

/* Scales the widget font (badges, status text). */
QFont scaledFont(const QFont &base, qreal factor, bool bold = false);

/* On / off switch (28 × 16). */
class Switch : public QAbstractButton {
	Q_OBJECT

public:
	explicit Switch(QWidget *parent = nullptr);
	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *) override;
};

/* Round status dot; hollow ring for errors. */
class StatusDot : public QWidget {
	Q_OBJECT

public:
	explicit StatusDot(QWidget *parent = nullptr);
	void setTone(core::Tone tone, bool ring);
	core::Tone tone() const { return tone_; }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	core::Tone tone_ = core::Tone::Idle;
	bool ring_ = false;
};

/* Rounded square in the platform colours with a glyph drawn in code. */
class PlatformTile : public QWidget {
	Q_OBJECT

public:
	PlatformTile(core::Platform platform, int size, int radius, QWidget *parent = nullptr);
	void setPlatform(core::Platform platform);
	core::Platform platform() const { return platform_; }

protected:
	void paintEvent(QPaintEvent *) override;

private:
	core::Platform platform_;
	int radius_;
};

/* Small filled label ("PROGRAM", "BETA", "LIVE"). */
QImage badgeImage(const QString &text, const QColor &fill, const QColor &textColor, const QFont &font, qreal dpr,
		  bool dot = false, int radius = 3);

/* Pill-shaped chip with text (canvas chip). */
class Chip : public QWidget {
	Q_OBJECT

public:
	explicit Chip(QWidget *parent = nullptr);
	void setText(const QString &text);
	QString text() const { return text_; }
	void setColors(const QColor &fill, const QColor &border, const QColor &textColor, int radius);
	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *) override;

private:
	QString text_;
	QColor fill_, border_, textColor_;
	int radius_ = 4;
};

/* "BETA" badge. */
class BetaBadge : public QWidget {
	Q_OBJECT

public:
	explicit BetaBadge(QWidget *parent = nullptr);
	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *) override;
};

} // namespace vertical::ui
