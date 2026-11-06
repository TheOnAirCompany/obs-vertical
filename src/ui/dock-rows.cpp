#include "ui/dock-rows.hpp"

#include "obs/text.hpp"
#include "ui/dock-decisions.hpp"
#include "ui/dock-style.hpp"

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

namespace vertical::ui {

/* ================================================================ ElidedLabel */

ElidedLabel::ElidedLabel(QWidget *parent) : QLabel(parent)
{
	setTextFormat(Qt::PlainText);
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void ElidedLabel::setFullText(const QString &text)
{
	if (text == full_)
		return;
	full_ = text;
	setToolTip(text);
	setAccessibleName(text);
	refresh();
}

QSize ElidedLabel::minimumSizeHint() const
{
	return QSize(20, QLabel::minimumSizeHint().height());
}

void ElidedLabel::resizeEvent(QResizeEvent *e)
{
	QLabel::resizeEvent(e);
	refresh();
}

void ElidedLabel::refresh()
{
	QLabel::setText(fontMetrics().elidedText(full_, Qt::ElideRight, qMax(0, width())));
}

/* ================================================================ SceneList */

SceneList::SceneList(QWidget *parent) : QListWidget(parent), rowHeight_(tokens::kRowStudio)
{
	setObjectName(QStringLiteral("verticalScenes"));
	setItemDelegate(new SceneDelegate(this));
	setDragDropMode(QAbstractItemView::InternalMove);
	setDefaultDropAction(Qt::MoveAction);
	setContextMenuPolicy(Qt::CustomContextMenu);
	setMouseTracking(true);
	setSpacing(1);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	setUniformItemSizes(true);
	setAccessibleName(T("Dock.Scenes"));
	restyle();
}

void SceneList::setRowHeight(int h)
{
	if (h == rowHeight_)
		return;
	rowHeight_ = h;
	/* Item sizes are cached by the view: relayout. */
	doItemsLayout();
	updateGeometry();
}

QRect SceneList::linkRect(const QRect &row) const
{
	const int hit = 18;
	return QRect(row.right() - 6 - hit + 1, row.center().y() - hit / 2, hit, hit);
}

QListWidgetItem *SceneList::itemNamed(const QString &scene) const
{
	for (int i = 0; i < count(); i++)
		if (item(i)->data(NameRole).toString() == scene)
			return item(i);
	return nullptr;
}

QRect SceneList::linkRectFor(const QString &scene) const
{
	QListWidgetItem *it = itemNamed(scene);
	return it ? linkRect(visualItemRect(it)) : QRect();
}

void SceneList::restyle()
{
	const Surfaces s = surfaces(this);
	const QString sheet =
		QStringLiteral("QListWidget#verticalScenes{background:%1;border:1px solid %2;border-radius:%3px;"
			       "padding:2px;outline:0;}")
			.arg(s.base.name(), s.line.name())
			.arg(tokens::kListRadius);
	if (sheet != styleSheet())
		setStyleSheet(sheet);
}

void SceneList::mousePressEvent(QMouseEvent *e)
{
	if (decisions::kLinkIconOnRows && e->button() == Qt::LeftButton) {
		const QModelIndex idx = indexAt(e->pos());
		if (idx.isValid()) {
			const QRect lr = linkRect(visualRect(idx));
			if (lr.contains(e->pos())) {
				emit linkRequested(idx.data(NameRole).toString(),
						   viewport()->mapToGlobal(lr.bottomLeft() + QPoint(0, 2)));
				e->accept();
				return;
			}
		}
	}
	QListWidget::mousePressEvent(e);
}

void SceneList::changeEvent(QEvent *e)
{
	QListWidget::changeEvent(e);
	if (e->type() == QEvent::ApplicationPaletteChange)
		restyle();
}

SceneDelegate::SceneDelegate(SceneList *list) : QStyledItemDelegate(list), list_(list) {}

QSize SceneDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
	return QSize(100, list_->rowHeight());
}

void SceneDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
	const Surfaces s = surfaces(list_);
	const QRect r = opt.rect;
	const bool program = index.data(SceneList::ProgramRole).toBool();
	const bool linked = index.data(SceneList::LinkedRole).toBool();
	const bool hover = opt.state & QStyle::State_MouseOver;
	const bool selected = opt.state & QStyle::State_Selected;
	const auto style = decisions::kProgramRow;
	const qreal dpr = list_->devicePixelRatioF();
	const qreal rad = tokens::kRowRadius;

	p->save();
	p->setRenderHint(QPainter::Antialiasing);
	const QRectF rr = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
	if (program && style == decisions::ProgramRow::Red) {
		QPainterPath clip;
		clip.addRoundedRect(QRectF(r), rad, rad);
		p->setClipPath(clip);
		p->fillRect(r, QColor::fromRgba(tokens::kProgramTint));
		p->fillRect(QRect(r.left(), r.top(), tokens::kProgramBar, r.height()), QColor::fromRgba(tokens::kRed));
		p->setClipping(false);
	} else if (program && style == decisions::ProgramRow::Blue) {
		p->setPen(Qt::NoPen);
		p->setBrush(QColor::fromRgba(tokens::kProgramBlue));
		p->drawRoundedRect(rr, rad, rad);
	} else if (program && style == decisions::ProgramRow::Outline) {
		p->setPen(QPen(QColor::fromRgba(tokens::kRed), 1.5));
		p->setBrush(Qt::NoBrush);
		p->drawRoundedRect(rr.adjusted(0.75, 0.75, -0.75, -0.75), rad, rad);
	} else if (selected || hover) {
		QColor c = s.row;
		if (!selected)
			c.setAlphaF(0.6f);
		p->setPen(Qt::NoPen);
		p->setBrush(c);
		p->drawRoundedRect(rr, rad, rad);
	}

	const bool onBlue = program && style == decisions::ProgramRow::Blue;
	const QColor text = onBlue ? QColor(Qt::white) : s.text;
	const QColor muted = onBlue ? QColor(Qt::white) : s.muted;
	const int cy = r.center().y();
	int x = r.left() + 6 + (program && style == decisions::ProgramRow::Red ? 2 : 0);

	/* Drag handle: two columns of three dots. */
	p->setPen(Qt::NoPen);
	p->setBrush(muted);
	for (int col = 0; col < 2; col++)
		for (int row = -1; row <= 1; row++)
			p->drawEllipse(QPointF(x + 2.5 + col * 4, cy + row * 4), 1.1, 1.1);
	x += 10 + 6;

	const int ic = 13;
	p->drawPixmap(QPoint(x, cy - ic / 2), iconPixmap(onBlue ? "play" : "screen", muted, ic, dpr));
	x += ic + 6;

	int right = r.right() - 6;
	if (decisions::kLinkIconOnRows) {
		const QRect lr = list_->linkRect(r);
		QColor lc = linked ? (onBlue ? QColor(Qt::white) : s.accent) : muted;
		if (!linked)
			lc.setAlphaF(0.35f);
		p->drawPixmap(QPoint(lr.center().x() - ic / 2, cy - ic / 2), iconPixmap("link", lc, ic, dpr));
		right = lr.left() - 6;
	}
	if (program && style != decisions::ProgramRow::Blue && style != decisions::ProgramRow::Outline) {
		if (r.width() >= tokens::kProgramBadgeMinRow) {
			const QImage badge = badgeImage(T("Dock.ProgramBadge"), QColor::fromRgba(tokens::kRed),
							Qt::white, scaledFont(opt.font, tokens::kBadgeText, true), dpr);
			const QSizeF bs = QSizeF(badge.size()) / dpr;
			right -= (int)bs.width();
			p->drawImage(QPointF(right, cy - bs.height() / 2), badge);
		} else {
			/* Narrow column: the name comes first, the badge becomes a red dot. */
			const qreal d = tokens::kProgramDot;
			right -= (int)d;
			p->setPen(Qt::NoPen);
			p->setBrush(QColor::fromRgba(tokens::kRed));
			p->drawEllipse(QRectF(right, cy - d / 2, d, d));
		}
		right -= 6;
	}

	const QString name = index.data(SceneList::NameRole).toString();
	QFont f = opt.font;
	f.setBold(program);
	p->setFont(f);
	p->setPen(text);
	const QFontMetrics fm(f);
	p->drawText(QRect(x, r.top(), qMax(0, right - x), r.height()), Qt::AlignVCenter | Qt::AlignLeft,
		    fm.elidedText(name, Qt::ElideRight, qMax(0, right - x)));
	p->restore();
}

/* ================================================================ buttons */

void styleStartStop(QToolButton *b, bool running)
{
	const Surfaces s = surfaces(b);
	b->setFixedSize(tokens::kButton, tokens::kButton);
	b->setIconSize(QSize(tokens::kIconSmall, tokens::kIconSmall));
	if (running) {
		b->setIcon(icon("stop", Qt::white, tokens::kIconSmall));
		setSheet(b, QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:5px;}"
					   "QToolButton:hover{border-color:%3;}")
				    .arg(QColor::fromRgba(tokens::kStopFill).name(),
					 QColor::fromRgba(tokens::kStopBorder).name(),
					 QColor::fromRgba(tokens::kRed).name()));
	} else {
		b->setIcon(icon("play", b->isEnabled() ? s.text : s.muted, tokens::kIconSmall));
		setSheet(b, QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:5px;}"
					   "QToolButton:hover{border-color:%3;}")
				    .arg(s.row.name(), s.line.name(), s.muted.name()));
	}
}

void restyleIconButton(QToolButton *b, const char *iconName, bool plain)
{
	const Surfaces s = surfaces(b);
	b->setIcon(icon(iconName, plain ? s.muted : s.text, tokens::kIcon));
	if (plain)
		setSheet(b, QStringLiteral("QToolButton{background:transparent;border:1px solid transparent;"
					   "border-radius:5px;}QToolButton:hover{background:%1;}"
					   "QToolButton::menu-indicator{image:none;width:0;}")
				    .arg(s.row.name()));
	else
		setSheet(b, QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:5px;}"
					   "QToolButton:hover{border-color:%3;}"
					   "QToolButton::menu-indicator{image:none;width:0;}")
				    .arg(s.row.name(), s.line.name(), s.muted.name()));
}

QToolButton *iconButton(const char *iconName, const QString &tip, int size, bool plain, QWidget *parent)
{
	auto *b = new QToolButton(parent);
	b->setFixedSize(size, size);
	b->setIconSize(QSize(tokens::kIcon, tokens::kIcon));
	b->setToolTip(tip);
	b->setAccessibleName(tip);
	restyleIconButton(b, iconName, plain);
	return b;
}

/* ================================================================ DestinationRow */

DestinationRow::DestinationRow(const std::string &id, core::Platform platform, const QString &name, bool compact,
			       QWidget *parent)
	: QWidget(parent),
	  id_(id),
	  compact_(compact)
{
	setObjectName(QString::fromStdString(id));
	auto *h = new QHBoxLayout(this);
	h->setContentsMargins(8, 6, 8, 6);
	h->setSpacing(8);
	const int tile = compact ? tokens::kTileCompact : tokens::kTile;
	tile_ = new PlatformTile(platform, tile, compact ? tokens::kTileCompactRadius : tokens::kTileRadius, this);
	h->addWidget(tile_);

	name_ = new ElidedLabel(this);
	name_->setObjectName(QStringLiteral("name"));
	name_->setFullText(name);
	dot_ = new StatusDot(this);
	if (compact) {
		/* Compact: dot and name on one line. */
		h->addWidget(dot_);
		h->addWidget(name_, 1);
		setMinimumHeight(tokens::kRowCompact);
	} else {
		QFont nf = name_->font();
		nf.setBold(true);
		name_->setFont(nf);
		auto *text = new QVBoxLayout();
		text->setSpacing(1);
		text->setContentsMargins(0, 0, 0, 0);
		text->addWidget(name_);
		auto *line = new QHBoxLayout();
		line->setSpacing(5);
		line->setContentsMargins(0, 0, 0, 0);
		status_ = new ElidedLabel(this);
		status_->setObjectName(QStringLiteral("status"));
		status_->setFont(scaledFont(font(), tokens::kSmallText));
		line->addWidget(dot_);
		line->addWidget(status_, 1);
		text->addLayout(line);
		h->addLayout(text, 1);
		setMinimumHeight(tokens::kRowStudio);
	}

	/* Settings before ▶ / ■: opens the same dialog as Edit in the ⋯ menu. */
	settings_ = iconButton("gear", T("Dock.DestinationSettings"), tokens::kButton, false, this);
	settings_->setObjectName(QStringLiteral("settings"));
	h->addWidget(settings_);
	connect(settings_, &QToolButton::clicked, this, [this] { emit editRequested(id_); });

	toggle_ = new QToolButton(this);
	toggle_->setObjectName(QStringLiteral("toggle"));
	h->addWidget(toggle_);
	connect(toggle_, &QToolButton::clicked, this, [this] { emit toggleRequested(id_); });
	if (!compact) {
		more_ = iconButton("more", T("Dock.More"), tokens::kSmallButton, true, this);
		more_->setObjectName(QStringLiteral("more"));
		h->addWidget(more_);
		connect(more_, &QToolButton::clicked, this,
			[this] { showMenu(more_->mapToGlobal(QPoint(0, more_->height()))); });
	}
	styleStartStop(toggle_, false);
}

void DestinationRow::setStatus(const core::StatusView &st, bool enabled)
{
	running_ = st.running;
	dot_->setTone(st.tone, st.ring);
	toggle_->setEnabled(enabled || st.running);
	styleStartStop(toggle_, st.running);
	toggle_->setToolTip(st.running ? T("Dock.Stop") : T("Dock.GoLive").arg(name_->fullText()));
	toggle_->setAccessibleName(st.running ? T("Dock.Stop") + QStringLiteral(" · ") + name_->fullText()
					      : toggle_->toolTip());
	const QString text = QString::fromStdString(st.text);
	statusText_ = text;
	if (status_) {
		status_->setFullText(enabled ? text : text + QStringLiteral(" · ") + T("Destination.Disabled"));
		QPalette pal = status_->palette();
		pal.setColor(QPalette::WindowText, st.tone == core::Tone::Error ? QColor::fromRgba(tokens::kErrorText)
										: surfaces(this).muted);
		status_->setPalette(pal);
	} else {
		dot_->setToolTip(text);
		name_->setToolTip(name_->fullText() + QStringLiteral(" · ") + text);
	}
}

QString DestinationRow::statusText() const
{
	return statusText_;
}

void DestinationRow::setSeparator(bool on)
{
	separator_ = on;
	update();
}

void DestinationRow::paintEvent(QPaintEvent *)
{
	if (!separator_)
		return;
	QPainter p(this);
	p.fillRect(QRect(0, 0, width(), 1), surfaces(this).line);
}

void DestinationRow::changeEvent(QEvent *e)
{
	QWidget::changeEvent(e);
	if (e->type() != QEvent::ApplicationPaletteChange)
		return;
	restyleIconButton(settings_, "gear", false);
	if (more_)
		restyleIconButton(more_, "more", true);
}

void DestinationRow::contextMenuEvent(QContextMenuEvent *e)
{
	showMenu(e->globalPos());
}

void DestinationRow::showMenu(const QPoint &globalPos)
{
	QMenu menu(this);
	menu.addAction(T("Destination.Edit"), this, [this] { emit editRequested(id_); });
	menu.addAction(T("Destination.Duplicate"), this, [this] { emit duplicateRequested(id_); });
	menu.addSeparator();
	menu.addAction(T("Destination.Delete"), this, [this] { emit deleteRequested(id_); });
	menu.exec(globalPos);
}

/* ================================================================ RoundIcon, AddRow */

RoundIcon::RoundIcon(const char *iconName, QRgb halo, QRgb color, QWidget *parent)
	: QWidget(parent),
	  icon_(iconName),
	  halo_(halo),
	  color_(color)
{
	setFixedSize(tokens::kRecDot, tokens::kRecDot);
}

void RoundIcon::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	/* halo 0: colours from the theme. */
	const Surfaces s = surfaces(this);
	const QColor halo = halo_ ? QColor::fromRgba(halo_) : s.raised;
	const QColor color = color_ ? QColor::fromRgba(color_) : s.text;
	p.setPen(Qt::NoPen);
	p.setBrush(halo);
	p.drawEllipse(rect());
	if (qstrcmp(icon_, "rec") == 0) {
		p.setBrush(color);
		p.drawEllipse(QRectF(width() / 2.0 - 7, height() / 2.0 - 7, 14, 14));
		return;
	}
	const int ic = 16;
	p.drawPixmap(QPoint((width() - ic) / 2, (height() - ic) / 2),
		     iconPixmap(icon_, color, ic, devicePixelRatioF()));
}

AddRow::AddRow(const QString &text, QWidget *parent) : QToolButton(parent)
{
	setText(text);
	setAccessibleName(text);
	setPopupMode(QToolButton::InstantPopup);
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setFixedHeight(tokens::kRowCompact);
}

void AddRow::paintEvent(QPaintEvent *)
{
	const Surfaces s = surfaces(this);
	QPainter p(this);
	QPen pen(s.line, 1, Qt::DashLine);
	p.setPen(pen);
	p.drawLine(0, 0, width(), 0);
	p.setPen(underMouse() ? s.text : s.muted);
	p.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, text());
}

} // namespace vertical::ui
