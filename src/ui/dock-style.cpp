#include "ui/dock-style.hpp"

#include "obs/text.hpp"
#include "ui/dock-decisions.hpp"

#include <QApplication>
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>

namespace vertical::ui {

namespace {

QColor mix(const QColor &a, const QColor &b, qreal t)
{
	return QColor::fromRgbF((float)(a.redF() + (b.redF() - a.redF()) * t),
				(float)(a.greenF() + (b.greenF() - a.greenF()) * t),
				(float)(a.blueF() + (b.blueF() - a.blueF()) * t));
}

/* 24 x 24 SVG bodies, stroke = currentColor (same drawings as the review page). */
const char *svgBody(const QByteArray &name)
{
	static const QHash<QByteArray, const char *> bodies = {
		{"play", "<path d='M7 4l13 8-13 8z' fill='currentColor' stroke='none'/>"},
		{"stop", "<rect x='5' y='5' width='14' height='14' rx='1.5' fill='currentColor' stroke='none'/>"},
		{"more", "<circle cx='5' cy='12' r='1.3' fill='currentColor'/><circle cx='12' cy='12' r='1.3' "
			 "fill='currentColor'/><circle cx='19' cy='12' r='1.3' fill='currentColor'/>"},
		{"link", "<path d='M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71'/><path d='M14 11a5 5 "
			 "0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71'/>"},
		{"screen", "<rect x='3' y='4' width='18' height='12' rx='2'/><path d='M8 20h8M12 16v4'/>"},
		{"plus", "<path d='M12 5v14M5 12h14'/>"},
		{"gear",
		 "<circle cx='12' cy='12' r='3'/><path d='M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 "
		 "2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 "
		 "19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 "
		 "0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 "
		 "2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 "
		 "1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 "
		 "0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z'/>"},
		{"chev", "<path d='m18 15-6-6-6 6'/>"},
		{"chevdown", "<path d='m6 9 6 6 6-6'/>"},
		{"rec", "<circle cx='12' cy='12' r='6' fill='currentColor' stroke='none'/>"},
		{"replay", "<path d='M3 12a9 9 0 1 0 3-6.7L3 8'/><path d='M3 3v5h5'/>"},
		{"cam", "<rect x='2' y='6' width='14' height='12' rx='2'/><path d='m16 10 6-4v12l-6-4'/>"},
		{"phone", "<rect x='7' y='2' width='10' height='20' rx='2'/>"},
		{"note", "<path d='M9 18V5l12-2v13'/><circle cx='6' cy='18' r='3'/><circle cx='18' cy='16' r='3'/>"},
		{"insta", "<rect x='4' y='4' width='16' height='16' rx='5'/><circle cx='12' cy='12' r='3.5'/>"},
		/* provisional: glyphs of custom servers and the detached window, same stroke style */
		{"server", "<rect x='3' y='4' width='18' height='7' rx='2'/><rect x='3' y='13' width='18' height='7' "
			   "rx='2'/><path d='M7 7.5h.01M7 16.5h.01'/>"},
		{"window", "<rect x='3' y='4' width='18' height='16' rx='2'/><path d='M3 9h18'/>"},
	};
	return bodies.value(name, nullptr);
}

QSvgRenderer *renderer(const char *name, const QColor &color)
{
	static QHash<QString, QSvgRenderer *> cache;
	const QString key = QString::fromUtf8(name) + QLatin1Char('|') + color.name(QColor::HexArgb);
	auto it = cache.constFind(key);
	if (it != cache.constEnd())
		return it.value();
	const char *body = svgBody(name);
	if (!body)
		return nullptr;
	QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
			 "stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>";
	svg += body;
	svg += "</svg>";
	svg.replace("currentColor", color.name(QColor::HexRgb).toUtf8());
	auto *r = new QSvgRenderer(svg);
	if (!r->isValid()) {
		delete r;
		r = nullptr;
	}
	cache.insert(key, r);
	return r;
}

} // namespace

Surfaces surfaces(const QWidget *w)
{
	Surfaces s;
	if (decisions::kColors == decisions::Colors::MockPalette) {
		s.panel = QColor(0x27, 0x2a, 0x33);
		s.base = QColor(0x1e, 0x20, 0x28);
		s.row = QColor(0x2e, 0x32, 0x3c);
		s.raised = QColor(0x35, 0x3a, 0x45);
		s.line = QColor(0x3a, 0x3f, 0x4b);
		s.text = QColor(0xe6, 0xe8, 0xee);
		s.muted = QColor(0x9a, 0xa0, 0xad);
		s.accent = QColor(0x18, 0xa0, 0xfb);
		return s;
	}
	/* The application palette of the OBS theme: widgets of the dock carry their own
	 * style sheets, which would feed back into their palette. */
	(void)w;
	const QPalette pal = QApplication::palette();
	s.panel = pal.color(QPalette::Window);
	s.base = pal.color(QPalette::Base);
	s.text = pal.color(QPalette::WindowText);
	s.row = pal.color(QPalette::Button);
	s.raised = mix(s.panel, s.text, tokens::kRaisedMix);
	s.line = mix(s.panel, s.text, tokens::kLineMix);
	s.muted = mix(s.text, s.panel, tokens::kMutedMix);
	s.accent = pal.color(QPalette::Highlight);
	return s;
}

QColor toneColor(core::Tone t)
{
	switch (t) {
	case core::Tone::Busy:
		return QColor::fromRgba(tokens::kAmber);
	case core::Tone::Live:
	case core::Tone::Error:
		return QColor::fromRgba(tokens::kRed);
	case core::Tone::Ok:
		return QColor::fromRgba(tokens::kGreen);
	default:
		return QColor::fromRgba(tokens::kIdle);
	}
}

QPixmap iconPixmap(const char *name, const QColor &color, int size, qreal dpr)
{
	QPixmap pm(QSize(size, size) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QSvgRenderer *r = renderer(name, color);
	if (!r)
		return pm;
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setOpacity(color.alphaF());
	r->render(&p, QRectF(0, 0, size, size));
	return pm;
}

QIcon icon(const char *name, const QColor &color, int size)
{
	QIcon ic;
	for (qreal dpr : {1.0, 2.0, 3.0})
		ic.addPixmap(iconPixmap(name, color, size, dpr));
	return ic;
}

bool iconsAvailable()
{
	return renderer("play", QColor(Qt::white)) != nullptr;
}

void setSheet(QWidget *w, const QString &sheet)
{
	if (w && w->styleSheet() != sheet)
		w->setStyleSheet(sheet);
}

QFont scaledFont(const QFont &base, qreal factor, bool bold)
{
	QFont f = base;
	if (f.pointSizeF() > 0)
		f.setPointSizeF(f.pointSizeF() * factor);
	else if (f.pixelSize() > 0)
		f.setPixelSize(qMax(8, qRound(f.pixelSize() * factor)));
	f.setBold(bold);
	return f;
}

/* ------------------------------------------------------------------ Switch */

Switch::Switch(QWidget *parent) : QAbstractButton(parent)
{
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setFocusPolicy(Qt::TabFocus);
}

QSize Switch::sizeHint() const
{
	return QSize(tokens::kSwitchW, tokens::kSwitchH);
}

void Switch::paintEvent(QPaintEvent *)
{
	const Surfaces s = surfaces(this);
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF r(0, (height() - tokens::kSwitchH) / 2.0, tokens::kSwitchW, tokens::kSwitchH);
	p.setPen(Qt::NoPen);
	p.setBrush(isChecked() ? s.accent : s.line);
	p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
	const qreal knob = r.height() - 4;
	const qreal x = isChecked() ? r.right() - 2 - knob : r.left() + 2;
	p.setBrush(isEnabled() ? QColor(Qt::white) : QColor(255, 255, 255, 120));
	p.drawEllipse(QRectF(x, r.top() + 2, knob, knob));
	if (hasFocus()) {
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(s.accent, 1));
		p.drawRoundedRect(r.adjusted(-1, -1, 1, 1), r.height() / 2 + 1, r.height() / 2 + 1);
	}
}

/* ------------------------------------------------------------------ StatusDot */

StatusDot::StatusDot(QWidget *parent) : QWidget(parent)
{
	setFixedSize(tokens::kDot + 2, tokens::kDot + 2);
	setAttribute(Qt::WA_TransparentForMouseEvents);
}

void StatusDot::setTone(core::Tone tone, bool ring)
{
	if (tone == tone_ && ring == ring_)
		return;
	tone_ = tone;
	ring_ = ring;
	update();
}

void StatusDot::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QColor c = toneColor(tone_);
	const QRectF r(1, 1, tokens::kDot, tokens::kDot);
	if (ring_) {
		p.setPen(QPen(c, 2));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(r.adjusted(1, 1, -1, -1));
	} else {
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawEllipse(r);
	}
}

/* ------------------------------------------------------------------ PlatformTile */

PlatformTile::PlatformTile(core::Platform platform, int size, int radius, QWidget *parent)
	: QWidget(parent),
	  platform_(platform),
	  radius_(radius)
{
	setFixedSize(size, size);
	setAttribute(Qt::WA_TransparentForMouseEvents);
}

void PlatformTile::setPlatform(core::Platform platform)
{
	platform_ = platform;
	update();
}

void PlatformTile::paintEvent(QPaintEvent *)
{
	const core::TileStyle st = core::tileStyle(platform_);
	const Surfaces s = surfaces(this);
	const bool mono = decisions::kTiles == decisions::Tiles::Mono || st.themed;
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF r = rect();
	if (mono) {
		p.setBrush(s.raised);
	} else if (st.stops > 1) {
		QLinearGradient g(r.topLeft(), r.bottomRight());
		g.setColorAt(0.0, QColor::fromRgb(st.colors[0]));
		g.setColorAt(st.stops == 3 ? 0.55 : 1.0, QColor::fromRgb(st.colors[1]));
		if (st.stops == 3)
			g.setColorAt(1.0, QColor::fromRgb(st.colors[2]));
		p.setBrush(g);
	} else {
		p.setBrush(QColor::fromRgb(st.colors[0]));
	}
	p.setPen(Qt::NoPen);
	p.drawRoundedRect(r, radius_, radius_);

	const QColor fg = mono ? s.text : QColor::fromRgb(st.foreground);
	const int glyph = qRound(width() * 15.0 / 28.0);
	const QRect gr((width() - glyph) / 2, (height() - glyph) / 2, glyph, glyph);
	switch (st.glyph) {
	case core::TileGlyph::LetterF: {
		QFont f = font();
		f.setBold(true);
		f.setWeight(QFont::Black);
		f.setPixelSize(qRound(width() * 15.0 / 28.0));
		p.setFont(f);
		p.setPen(fg);
		p.drawText(rect(), Qt::AlignCenter, QStringLiteral("f"));
		break;
	}
	case core::TileGlyph::Srt: {
		QFont f = font();
		f.setBold(true);
		f.setPixelSize(qMax(7, qRound(width() * 9.0 / 28.0)));
		p.setFont(f);
		p.setPen(fg);
		p.drawText(rect(), Qt::AlignCenter, QStringLiteral("SRT"));
		break;
	}
	default: {
		const char *name = st.glyph == core::TileGlyph::Note     ? "note"
				   : st.glyph == core::TileGlyph::Play   ? "play"
				   : st.glyph == core::TileGlyph::Camera ? "insta"
									 : "server";
		p.drawPixmap(gr.topLeft(), iconPixmap(name, fg, glyph, devicePixelRatioF()));
		break;
	}
	}
}

/* ------------------------------------------------------------------ badges and chips */

QImage badgeImage(const QString &text, const QColor &fill, const QColor &textColor, const QFont &font, qreal dpr,
		  bool dot, int radius)
{
	const QFontMetrics fm(font);
	const int padX = 5, padY = 1;
	const int dotW = dot ? 6 + 4 : 0;
	const QSize size(fm.horizontalAdvance(text) + padX * 2 + dotW, fm.height() + padY * 2);
	QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
	img.setDevicePixelRatio(dpr);
	img.fill(Qt::transparent);
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(fill);
	p.drawRoundedRect(QRectF(0, 0, size.width(), size.height()), radius, radius);
	if (dot) {
		p.setBrush(textColor);
		p.drawEllipse(QRectF(padX, (size.height() - 6) / 2.0, 6, 6));
	}
	p.setFont(font);
	p.setPen(textColor);
	p.drawText(QRect(padX + dotW, 0, size.width() - padX * 2 - dotW, size.height()), Qt::AlignCenter, text);
	return img;
}

Chip::Chip(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

void Chip::setText(const QString &text)
{
	if (text == text_)
		return;
	text_ = text;
	updateGeometry();
	update();
}

void Chip::setColors(const QColor &fill, const QColor &border, const QColor &textColor, int radius)
{
	fill_ = fill;
	border_ = border;
	textColor_ = textColor;
	radius_ = radius;
	update();
}

QSize Chip::sizeHint() const
{
	const QFontMetrics fm(font());
	return QSize(fm.horizontalAdvance(text_) + 16, fm.height() + 4);
}

void Chip::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	p.setPen(QPen(border_, 1));
	p.setBrush(fill_);
	p.drawRoundedRect(r, radius_, radius_);
	p.setPen(textColor_);
	p.drawText(rect(), Qt::AlignCenter, fontMetrics().elidedText(text_, Qt::ElideRight, width() - 8));
}

BetaBadge::BetaBadge(QWidget *parent) : QWidget(parent)
{
	setFont(scaledFont(font(), tokens::kBadgeText, true));
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	setObjectName(QStringLiteral("betaBadge"));
	setAccessibleName(T("Dock.Beta"));
	setToolTip(T("VirtualCam.BetaTip"));
}

QSize BetaBadge::sizeHint() const
{
	const QFontMetrics fm(font());
	return QSize(fm.horizontalAdvance(T("Dock.Beta")) + 8, fm.height() + 2);
}

void BetaBadge::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor::fromRgba(tokens::kAmber));
	p.drawRoundedRect(rect(), 3, 3);
	p.setPen(QColor::fromRgba(tokens::kBetaText));
	p.drawText(rect(), Qt::AlignCenter, T("Dock.Beta"));
}

} // namespace vertical::ui
