#include "ui/dock.hpp"

#include "obs/hotkeys.hpp"
#include "obs/outputs.hpp"
#include "obs/text.hpp"
#include "obs/vertical.hpp"
#include "obs/virtualcam.hpp"
#include "ui/destination-dialog.hpp"
#include "ui/dock-decisions.hpp"
#include "ui/dock-rows.hpp"
#include "ui/dock-style.hpp"
#include "ui/preview.hpp"
#include "ui/settings-dialog.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/config-file.h>

#include <QAbstractButton>
#include <QActionGroup>
#include <QDateTime>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace vertical::ui {

namespace {

/* provisional: values the review page does not give. */
constexpr int kLineHeight = tokens::kButton + 4;    /* Compact lines hold a 28 px button */
constexpr int kNarrowOutputs = 380;                 /* below: Start all / Stop all show icons only */
constexpr int kFooterVersionMin = 600;              /* below: the footer drops the version */
constexpr int kFooterEncoderLabelMin = 560;         /* below: the footer drops "Shared encoder" */
constexpr int kHeaderLabelMin = 400;                /* below: the header keeps the switch, not its label */
constexpr int kBigButtonMin = 100;                  /* Stop Recording may clip its text below this */
constexpr int kVirtualCamPollSeconds = 3;           /* re-check the extension while not running */
constexpr int kTabHeight = 26;                      /* segmented tabs, 5 px padding in the mock */
constexpr const char *kConfigSection = "Vertical";  /* OBS user config */
constexpr const char *kConfigLayout = "DockLayout"; /* "auto" | "compact" | "studio" */

core::Phase phaseOf(OutputState s)
{
	switch (s) {
	case OutputState::Starting:
		return core::Phase::Connecting;
	case OutputState::Live:
		return core::Phase::Live;
	case OutputState::Reconnecting:
		return core::Phase::Reconnecting;
	case OutputState::Stopping:
		return core::Phase::Stopping;
	case OutputState::Error:
		return core::Phase::Error;
	default:
		return core::Phase::Stopped;
	}
}

QString qs(const std::string &s)
{
	return QString::fromStdString(s);
}

std::vector<std::string> mainSceneNames()
{
	std::vector<std::string> out;
	char **names = obs_frontend_get_scene_names();
	for (char **n = names; n && *n; n++)
		out.emplace_back(*n);
	bfree(names);
	return out;
}

QFrame *hline(QWidget *parent)
{
	auto *f = new QFrame(parent);
	f->setObjectName(QStringLiteral("dockLine"));
	f->setFixedHeight(1);
	return f;
}

QFrame *vline(QWidget *parent)
{
	auto *f = new QFrame(parent);
	f->setObjectName(QStringLiteral("dockLine"));
	f->setFixedWidth(1);
	return f;
}

QLabel *sectionTitle(const QString &text, QWidget *parent)
{
	auto *l = new QLabel(text, parent);
	QFont f = l->font();
	f.setBold(true);
	l->setFont(f);
	return l;
}

QLabel *mutedSmall(QWidget *parent, bool mono = false)
{
	auto *l = new QLabel(parent);
	QFont f = scaledFont(l->font(), tokens::kSmallText);
	if (mono) {
		QFont m = QFontDatabase::systemFont(QFontDatabase::FixedFont);
		m.setPointSizeF(f.pointSizeF() > 0 ? f.pointSizeF() : 10.0);
		f = m;
	}
	l->setFont(f);
	l->setObjectName(QStringLiteral("muted"));
	return l;
}

void setMuted(QLabel *l, const QColor &c)
{
	QPalette p = l->palette();
	p.setColor(QPalette::WindowText, c);
	l->setPalette(p);
}

/* Big button of the recording tabs: red "Stop" while running, neutral otherwise. */
void styleBigButton(QPushButton *b, bool running, const char *idleIcon, QRgb idleIconColor)
{
	const Surfaces s = surfaces(b);
	b->setFixedHeight(tokens::kBigButton);
	QFont f = b->font();
	f.setBold(true);
	b->setFont(f);
	if (running) {
		b->setIcon(icon("stop", Qt::white, 10));
		setSheet(b, QStringLiteral("QPushButton{background:%1;color:white;border:none;border-radius:6px;"
					   "padding:0 10px;}QPushButton:hover{background:%2;}")
				    .arg(QColor::fromRgba(tokens::kBigStop).name(),
					 QColor::fromRgba(tokens::kRed).name()));
	} else {
		b->setIcon(icon(idleIcon, idleIconColor ? QColor::fromRgba(idleIconColor) : s.text, 12));
		setSheet(b, QStringLiteral("QPushButton{background:%1;color:%2;border:1px solid %3;"
					   "border-radius:6px;padding:0 10px;}QPushButton:hover{border-color:%4;}"
					   "QPushButton:disabled{color:%4;}")
				    .arg(s.row.name(), s.text.name(), s.line.name(), s.muted.name()));
	}
}

/* Keeps the menu open when a checkable entry is clicked (several links in a row). */
class StickyMenu : public QMenu {
public:
	using QMenu::QMenu;

protected:
	void mouseReleaseEvent(QMouseEvent *e) override
	{
		QAction *a = activeAction();
		if (a && a->isEnabled() && a->isCheckable()) {
			a->trigger();
			return;
		}
		QMenu::mouseReleaseEvent(e);
	}
};

/* Segment of the Recording | Replay Buffer | Virtual Camera bar. */
class TabButton : public QAbstractButton {
public:
	TabButton(const char *iconName, const QString &text, bool beta, QWidget *parent)
		: QAbstractButton(parent),
		  icon_(iconName),
		  beta_(beta)
	{
		setText(text);
		setToolTip(text);
		setAccessibleName(text);
		setCheckable(true);
		setAutoExclusive(true);
		/* Own font: the style sheet of the tab bar re-polishes the widget font. */
		font_ = scaledFont(font(), tokens::kSmallText);
		font_.setWeight(QFont::Medium);
		setCursor(Qt::PointingHandCursor);
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		setFixedHeight(kTabHeight);
	}
	QSize sizeHint() const override { return QSize(contentWidth() + 8, kTabHeight); }
	QSize minimumSizeHint() const override { return QSize(tokens::kIconSmall + 12, kTabHeight); }

protected:
	int badgeWidth() const
	{
		return beta_ ? QFontMetrics(scaledFont(font_, tokens::kBadgeText, true))
					       .horizontalAdvance(T("Dock.Beta")) +
				       8 + 5
			     : 0;
	}
	int contentWidth() const
	{
		return tokens::kIconSmall + 5 + QFontMetrics(font_).horizontalAdvance(text()) + badgeWidth();
	}
	void paintEvent(QPaintEvent *) override
	{
		const Surfaces s = surfaces(this);
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		if (isChecked()) {
			p.setPen(QPen(s.line, 1));
			p.setBrush(s.raised);
			p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
		}
		const QColor fg = isChecked() ? s.text : s.muted;
		const int avail = width() - 8;
		const QFontMetrics fm(font_);
		int textW = fm.horizontalAdvance(text());
		int badge = badgeWidth();
		int total = tokens::kIconSmall + 5 + textW + badge;
		if (total > avail) {
			badge = 0;
			textW = std::max(0, avail - tokens::kIconSmall - 5);
			total = tokens::kIconSmall + (textW > 0 ? 5 + textW : 0);
		}
		int x = (width() - total) / 2;
		const int cy = height() / 2;
		p.drawPixmap(QPoint(x, cy - tokens::kIconSmall / 2),
			     iconPixmap(icon_, fg, tokens::kIconSmall, devicePixelRatioF()));
		x += tokens::kIconSmall + 5;
		if (textW > 0) {
			p.setFont(font_);
			p.setPen(fg);
			p.drawText(QRect(x, 0, textW, height()), Qt::AlignVCenter | Qt::AlignLeft,
				   fm.elidedText(text(), Qt::ElideRight, textW));
			x += textW;
		}
		if (badge > 0) {
			const QImage img = badgeImage(T("Dock.Beta"), QColor::fromRgba(tokens::kAmber),
						      QColor::fromRgba(tokens::kBetaText),
						      scaledFont(font_, tokens::kBadgeText, true), devicePixelRatioF());
			const QSizeF bs = QSizeF(img.size()) / devicePixelRatioF();
			p.drawImage(QPointF(x + 5, cy - bs.height() / 2), img);
		}
	}

private:
	const char *icon_;
	bool beta_;
	QFont font_;
};

QFrame *lineFrame(QWidget *parent)
{
	auto *f = new QFrame(parent);
	f->setObjectName(QStringLiteral("dockLineBox"));
	f->setFixedHeight(kLineHeight);
	return f;
}

QLabel *iconLabel(QWidget *parent, int size)
{
	auto *l = new QLabel(parent);
	l->setFixedSize(size, size);
	return l;
}

} // namespace

/* ================================================================ construction */

Dock::Dock(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("verticalDockContent"));
	Vertical &v = Vertical::instance();
	Outputs &o = Outputs::instance();

	/* Layout choice, kept in the OBS user config (global, not per collection). */
	if (decisions::kModes != decisions::Modes::Automatic) {
		config_t *cfg = obs_frontend_get_user_config();
		const char *id = cfg ? config_get_string(cfg, kConfigSection, kConfigLayout) : nullptr;
		choice_ = core::layoutChoiceFromId(id ? id : "");
		if (decisions::kModes == decisions::Modes::Manual && choice_ == core::LayoutChoice::Automatic)
			choice_ = core::LayoutChoice::Studio;
	}

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	header_ = buildHeader();
	root->addWidget(header_);
	root->addWidget(hline(this));
	header_->setVisible(decisions::kHeader == decisions::Header::CanvasLine);

	body_ = new QWidget(this);
	bodyLayout_ = new QHBoxLayout(body_);
	bodyLayout_->setContentsMargins(0, 0, 0, 0);
	bodyLayout_->setSpacing(0);
	colA_ = new QWidget(body_);
	colALayout_ = new QVBoxLayout(colA_);
	colALayout_->setContentsMargins(tokens::kPadding, tokens::kPadding, tokens::kPadding, tokens::kPadding);
	colALayout_->setSpacing(tokens::kGap);
	divider_ = vline(body_);
	colB_ = new QWidget(body_);
	colBLayout_ = new QVBoxLayout(colB_);
	colBLayout_->setContentsMargins(tokens::kPadding, tokens::kPadding, tokens::kPadding, tokens::kPadding);
	colBLayout_->setSpacing(tokens::kGap);
	bodyLayout_->addWidget(colA_);
	bodyLayout_->addWidget(divider_);
	bodyLayout_->addWidget(colB_, 1);
	root->addWidget(body_, 1);

	/* Preview. */
	preview_ = new PreviewFrame(colA_);
	preview_->setCollapsed(!v.settings().previewVisible);

	/* Scenes. */
	scenesHeader_ = buildScenesHeader();
	scenes_ = new SceneList(colA_);
	scenes_->setMinimumHeight(tokens::kScenesMinRows * tokens::kRowCompact + 8);

	/* Outputs. */
	outputsHeader_ = buildOutputsHeader();
	destinationsBox_ = static_cast<QFrame *>(buildDestinations());
	tabsBox_ = buildTabs();
	linesBox_ = buildLines();

	root->addWidget(hline(this));
	footer_ = buildFooter();
	root->addWidget(footer_);

	/* ---- wiring ---- */
	connect(linked_, &Switch::toggled, this, [](bool on) { Vertical::instance().setLinkedMode(on); });
	connect(scenes_, &QListWidget::itemClicked, this, [](QListWidgetItem *item) {
		Vertical::instance().setCurrentScene(item->data(SceneList::NameRole).toString());
	});
	connect(scenes_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { renameScene(); });
	connect(scenes_, &QListWidget::customContextMenuRequested, this, &Dock::onSceneMenu);
	connect(scenes_, &SceneList::linkRequested, this,
		[this](const QString &scene, const QPoint &pos) { openLinkMenu(scene, pos); });
	connect(scenes_->model(), &QAbstractItemModel::rowsMoved, this, [this] {
		if (rebuilding_)
			return;
		/* Drag and drop reorder: store the new order. */
		std::vector<std::string> order;
		for (int i = 0; i < scenes_->count(); i++)
			order.push_back(scenes_->item(i)->data(SceneList::NameRole).toString().toStdString());
		Vertical::instance().settings().sceneOrder = order;
	});
	connect(preview_, &PreviewFrame::collapsedChanged, this,
		[](bool collapsed) { Vertical::instance().settings().previewVisible = !collapsed; });
	connect(preview_, &PreviewFrame::detachRequested, this, &Dock::openPreviewWindow);

	connect(&v, &Vertical::canvasChanged, this, [this] {
		preview_->preview()->setCanvas(Vertical::instance().canvas());
		if (previewWindow_)
			previewWindow_->preview()->setCanvas(Vertical::instance().canvas());
		refreshHeader();
	});
	connect(&v, &Vertical::scenesChanged, this, &Dock::rebuildScenes);
	connect(&v, &Vertical::currentSceneChanged, this, &Dock::rebuildScenes);
	connect(&v, &Vertical::linksChanged, this, &Dock::rebuildScenes);
	connect(&v, &Vertical::settingsChanged, this, [this] {
		preview_->setCollapsed(!Vertical::instance().settings().previewVisible);
		{
			const QSignalBlocker block(linked_);
			linked_->setChecked(Vertical::instance().linkedMode());
		}
		rebuildDestinations();
		refreshHeader();
	});
	connect(&v, &Vertical::renderingChanged, this, [this](bool) { refreshHeader(); });
	connect(&o, &Outputs::statusChanged, this, &Dock::refreshStatus);
	connect(&o, &Outputs::outputError, this, &Dock::showError);
	connect(&o, &Outputs::replaySaved, this, [this](const QString &path) {
		replayInfo_->setFullText(T("Replay.Saved").arg(path));
		replayInfo_->show();
	});

	cpuInfo_ = os_cpu_usage_info_start();
	clock_.setInterval(1000);
	connect(&clock_, &QTimer::timeout, this, &Dock::refreshTimers);
	clock_.start();

#ifdef VERTICAL_VIRTUALCAM
	vcamInstalled_ = virtualcam::isExtensionInstalled();
#endif

	preview_->preview()->setCanvas(v.canvas());
	applyColors();
	/* Automatic: Compact until the first resize gives the real width. */
	applyMode(choice_ == core::LayoutChoice::Studio ? core::DockMode::Studio : core::DockMode::Compact);
	rebuildScenes();
	refreshHeader();
	refreshStatus();
}

Dock::~Dock()
{
	if (cpuInfo_)
		os_cpu_usage_info_destroy(cpuInfo_);
	if (previewWindow_)
		delete previewWindow_.data();
}

QWidget *Dock::buildHeader()
{
	auto *w = new QWidget(this);
	w->setObjectName(QStringLiteral("dockHeader"));
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(tokens::kPadding, 7, tokens::kPadding, 7);
	h->setSpacing(8);
	phoneIcon_ = iconLabel(w, 14);
	canvasChip_ = new Chip(w);
	canvasChip_->setObjectName(QStringLiteral("canvasChip"));
	QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	const QFont small = scaledFont(font(), tokens::kSmallText);
	mono.setPointSizeF(small.pointSizeF() > 0 ? small.pointSizeF() : 10.0);
	canvasChip_->setFont(mono);
	linkedLabel_ = mutedSmall(w);
	linkedLabel_->setText(T("Scene.LinkedMode"));
	linked_ = new Switch(w);
	linked_->setObjectName(QStringLiteral("linkedSwitch"));
	linked_->setToolTip(T("Scene.LinkedModeTip"));
	linked_->setAccessibleName(T("Scene.LinkedMode"));
	linked_->setChecked(Vertical::instance().linkedMode());
	linkedLabel_->setToolTip(T("Scene.LinkedModeTip"));
	more_ = iconButton("more", T("Dock.More"), tokens::kSmallButton, true, w);
	more_->setObjectName(QStringLiteral("dockMore"));
	more_->setPopupMode(QToolButton::InstantPopup);
	moreMenu_ = buildMoreMenu();
	more_->setMenu(moreMenu_);
	h->addWidget(phoneIcon_);
	h->addWidget(canvasChip_);
	h->addStretch(1);
	h->addWidget(linkedLabel_);
	h->addWidget(linked_);
	h->addWidget(more_);
	return w;
}

QMenu *Dock::buildMoreMenu()
{
	auto *m = new QMenu(this);
	m->setObjectName(QStringLiteral("dockMoreMenu"));
	if (decisions::kModes != decisions::Modes::Automatic) {
		QMenu *layout = m->addMenu(T("Dock.Layout"));
		auto *group = new QActionGroup(layout);
		const auto add = [&](core::LayoutChoice c, const char *key) {
			QAction *a = layout->addAction(T(key));
			a->setCheckable(true);
			a->setData((int)c);
			group->addAction(a);
			connect(a, &QAction::triggered, this, [this, c] { setLayoutChoice(c); });
		};
		if (decisions::kModes == decisions::Modes::AutomaticOverridable)
			add(core::LayoutChoice::Automatic, "Dock.Layout.Auto");
		add(core::LayoutChoice::Compact, "Dock.Layout.Compact");
		add(core::LayoutChoice::Studio, "Dock.Layout.Studio");
		connect(layout, &QMenu::aboutToShow, this, [this, group] {
			for (QAction *a : group->actions())
				a->setChecked(a->data().toInt() == (int)choice_);
		});
	}
	QAction *showPreview = m->addAction(T("Dock.ShowPreview"));
	showPreview->setCheckable(true);
	connect(showPreview, &QAction::triggered, this, [this](bool on) { preview_->setCollapsed(!on); });
	m->addAction(T("Dock.OpenPreviewWindow"), this, &Dock::openPreviewWindow);
	m->addSeparator();
	m->addAction(T("Scene.Add"), this, &Dock::addScene);
	m->addAction(T("Dock.StartAll"), this, [] { Outputs::instance().startAll(); });
	m->addAction(T("Dock.StopAll"), this, [] { Outputs::instance().stopAllStreams(); });
	m->addSeparator();
	m->addAction(T("Dock.Settings"), this, [this] { openSettings(); });
	connect(m, &QMenu::aboutToShow, this, [this, showPreview] { showPreview->setChecked(!preview_->collapsed()); });
	return m;
}

QWidget *Dock::buildScenesHeader()
{
	auto *w = new QWidget(colA_);
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(6);
	h->addWidget(sectionTitle(T("Dock.Scenes"), w));
	scenesCount_ = mutedSmall(w, true);
	h->addWidget(scenesCount_);
	h->addStretch(1);
	if (decisions::kHeader == decisions::Header::None) {
		/* No header line: its controls live here. */
		h->addWidget(linkedLabel_);
		h->addWidget(linked_);
		h->addWidget(more_);
	}
	addSceneButton_ = iconButton("plus", T("Scene.Add"), tokens::kSmallButton, false, w);
	addSceneButton_->setObjectName(QStringLiteral("addScene"));
	connect(addSceneButton_, &QToolButton::clicked, this, &Dock::addScene);
	h->addWidget(addSceneButton_);
	return w;
}

QWidget *Dock::buildOutputsHeader()
{
	auto *w = new QWidget(colB_);
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(6);
	h->addWidget(sectionTitle(T("Dock.Outputs"), w));
	outputsCount_ = mutedSmall(w, true);
	outputsCount_->setObjectName(QStringLiteral("outputsCount"));
	h->addWidget(outputsCount_);
	h->addStretch(1);
	startAll_ = new QPushButton(T("Dock.StartAll"), w);
	startAll_->setObjectName(QStringLiteral("startAll"));
	stopAll_ = new QPushButton(T("Dock.StopAll"), w);
	stopAll_->setObjectName(QStringLiteral("stopAll"));
	for (QPushButton *b : {startAll_, stopAll_}) {
		b->setFixedHeight(tokens::kButton);
		b->setIconSize(QSize(11, 11));
		b->setToolTip(b->text());
		h->addWidget(b);
	}
	connect(startAll_, &QPushButton::clicked, this, [] { Outputs::instance().startAll(); });
	connect(stopAll_, &QPushButton::clicked, this, [] { Outputs::instance().stopAllStreams(); });
	return w;
}

QWidget *Dock::buildDestinations()
{
	auto *box = new QFrame(this);
	box->setObjectName(QStringLiteral("destinations"));
	auto *v = new QVBoxLayout(box);
	v->setContentsMargins(1, 1, 1, 1);
	v->setSpacing(0);
	destinationsScroll_ = new QScrollArea(box);
	destinationsScroll_->setFrameShape(QFrame::NoFrame);
	destinationsScroll_->setWidgetResizable(true);
	destinationsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	destinationsScroll_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
	destinationsScroll_->viewport()->setAutoFillBackground(false);
	destinationsList_ = new QWidget(destinationsScroll_);
	destinationsList_->setAutoFillBackground(false);
	destinationsLayout_ = new QVBoxLayout(destinationsList_);
	destinationsLayout_->setContentsMargins(0, 0, 0, 0);
	destinationsLayout_->setSpacing(0);
	destinationsScroll_->setWidget(destinationsList_);
	v->addWidget(destinationsScroll_);

	noDestinations_ = new QLabel(T("Dock.NoDestinations"), box);
	noDestinations_->setWordWrap(true);
	noDestinations_->setContentsMargins(8, 6, 8, 6);
	noDestinations_->setFont(scaledFont(font(), tokens::kSmallText));
	v->addWidget(noDestinations_);

	auto *add = new AddRow(QStringLiteral("+ ") + T("Dock.AddDestination"), box);
	add->setObjectName(QStringLiteral("addDestination"));
	auto *menu = new QMenu(add);
	for (core::Platform p : core::allPlatforms())
		menu->addAction(QString::fromUtf8(core::platformInfo(p).displayName), this,
				[this, p] { addDestination(p); });
	add->setMenu(menu);
	addDestination_ = add;
	v->addWidget(add);
	return box;
}

QWidget *Dock::buildTabs()
{
	auto *w = new QWidget(this);
	w->setObjectName(QStringLiteral("outputTabs"));
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(tokens::kGap);
	auto *bar = new QFrame(w);
	bar->setObjectName(QStringLiteral("tabsBar"));
	auto *h = new QHBoxLayout(bar);
	h->setContentsMargins(3, 3, 3, 3);
	h->setSpacing(2);
	const bool beta = decisions::kVirtualCamBeta == decisions::VirtualCamBeta::Badge;
	tabButtons_[0] = new TabButton("rec", T("Dock.Tab.Recording"), false, bar);
	tabButtons_[1] = new TabButton("replay", T("Dock.Tab.Replay"), false, bar);
	tabButtons_[2] = new TabButton("cam", T("Dock.Tab.VirtualCam"), beta, bar);
	tabStack_ = new QStackedWidget(w);
	tabStack_->addWidget(buildRecordPanel());
	tabStack_->addWidget(buildReplayPanel());
	tabStack_->addWidget(buildVirtualCamPanel());
	for (int i = 0; i < 3; i++) {
		tabButtons_[i]->setObjectName(QStringLiteral("tab%1").arg(i));
		h->addWidget(tabButtons_[i]);
		connect(tabButtons_[i], &QAbstractButton::toggled, this, [this, i](bool on) {
			if (on)
				tabStack_->setCurrentIndex(i);
		});
	}
	tabButtons_[0]->setChecked(true);
#ifndef VERTICAL_VIRTUALCAM
	tabButtons_[2]->hide();
#endif
	v->addWidget(bar);
	v->addWidget(tabStack_);
	return w;
}

QWidget *Dock::buildRecordPanel()
{
	auto *w = new QWidget(this);
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(8);
	auto *top = new QHBoxLayout();
	top->setSpacing(10);
	top->addWidget(new RoundIcon("rec", tokens::kRecHalo, tokens::kRed, w));
	auto *text = new QVBoxLayout();
	text->setSpacing(1);
	text->addWidget(sectionTitle(T("Dock.RecordingTitle"), w));
	recordDetail_ = new ElidedLabel(w);
	recordDetail_->setObjectName(QStringLiteral("muted"));
	recordDetail_->setFont(scaledFont(font(), tokens::kSmallText));
	text->addWidget(recordDetail_);
	top->addLayout(text, 1);
	auto *gear = iconButton("gear", T("Dock.RecordingSettings"), tokens::kSmallButton, false, w);
	gear->setObjectName(QStringLiteral("recordSettings"));
	connect(gear, &QToolButton::clicked, this, [this] { openSettings((int)SettingsDialog::Tab::Recording); });
	top->addWidget(gear);
	v->addLayout(top);

	auto *row = new QHBoxLayout();
	row->setSpacing(8);
	recordButton_ = new QPushButton(T("Dock.StartRecording"), w);
	recordButton_->setObjectName(QStringLiteral("recordButton"));
	recordTimer_ = new QLabel(QStringLiteral("00:00:00"), w);
	recordTimer_->setObjectName(QStringLiteral("recordTimer"));
	recordTimer_->setFixedHeight(tokens::kBigButton);
	QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	mono.setPointSizeF(font().pointSizeF() > 0 ? font().pointSizeF() : 11.0);
	recordTimer_->setFont(mono);
	recordTimer_->setAlignment(Qt::AlignCenter);
	recordButton_->setMinimumWidth(kBigButtonMin);
	row->addWidget(recordButton_, 1);
	row->addWidget(recordTimer_);
	v->addLayout(row);
	recordError_ = new ElidedLabel(w);
	recordError_->setFont(scaledFont(font(), tokens::kSmallText));
	recordError_->hide();
	v->addWidget(recordError_);
	v->addStretch(1);
	connect(recordButton_, &QPushButton::clicked, this, &Dock::toggleRecording);
	return w;
}

QWidget *Dock::buildReplayPanel()
{
	auto *w = new QWidget(this);
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(8);
	auto *top = new QHBoxLayout();
	top->setSpacing(10);
	top->addWidget(new RoundIcon("replay", 0, 0, w));
	auto *text = new QVBoxLayout();
	text->setSpacing(1);
	text->addWidget(sectionTitle(T("Dock.Replay"), w));
	replayDetail_ = new ElidedLabel(w);
	replayDetail_->setObjectName(QStringLiteral("muted"));
	replayDetail_->setFont(scaledFont(font(), tokens::kSmallText));
	text->addWidget(replayDetail_);
	top->addLayout(text, 1);
	auto *gear = iconButton("gear", T("Dock.RecordingSettings"), tokens::kSmallButton, false, w);
	connect(gear, &QToolButton::clicked, this, [this] { openSettings((int)SettingsDialog::Tab::Recording); });
	top->addWidget(gear);
	v->addLayout(top);

	auto *row = new QHBoxLayout();
	row->setSpacing(8);
	replayButton_ = new QPushButton(T("Dock.StartReplay"), w);
	replayButton_->setObjectName(QStringLiteral("replayButton"));
	saveReplay_ = new QPushButton(T("Dock.SaveReplay"), w);
	saveReplay_->setObjectName(QStringLiteral("saveReplay"));
	replayButton_->setMinimumWidth(kBigButtonMin);
	row->addWidget(replayButton_, 1);
	row->addWidget(saveReplay_);
	v->addLayout(row);
	replayInfo_ = new ElidedLabel(w);
	replayInfo_->setFont(scaledFont(font(), tokens::kSmallText));
	replayInfo_->hide();
	v->addWidget(replayInfo_);
	v->addStretch(1);
	connect(replayButton_, &QPushButton::clicked, this, &Dock::toggleReplay);
	connect(saveReplay_, &QPushButton::clicked, this, [] { Outputs::instance().saveReplay(); });
	return w;
}

QWidget *Dock::buildVirtualCamPanel()
{
	auto *w = new QWidget(this);
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(8);
	auto *top = new QHBoxLayout();
	top->setSpacing(10);
	top->addWidget(new RoundIcon("cam", 0, 0, w));
	auto *text = new QVBoxLayout();
	text->setSpacing(1);
	auto *titleRow = new QHBoxLayout();
	titleRow->setSpacing(6);
	titleRow->addWidget(sectionTitle(T("VirtualCam.Name"), w));
	if (decisions::kVirtualCamBeta == decisions::VirtualCamBeta::Badge)
		titleRow->addWidget(new BetaBadge(w));
	titleRow->addStretch(1);
	text->addLayout(titleRow);
	auto *statusRow = new QHBoxLayout();
	statusRow->setSpacing(5);
	vcamDot_ = new StatusDot(w);
	vcamStatus_ = new ElidedLabel(w);
	vcamStatus_->setObjectName(QStringLiteral("vcamStatus"));
	vcamStatus_->setFont(scaledFont(font(), tokens::kSmallText));
	statusRow->addWidget(vcamDot_);
	statusRow->addWidget(vcamStatus_, 1);
	text->addLayout(statusRow);
	top->addLayout(text, 1);
	v->addLayout(top);
	vcamButton_ = new QPushButton(w);
	vcamButton_->setObjectName(QStringLiteral("vcamButton"));
	v->addWidget(vcamButton_);
	vcamNote_ = mutedSmall(w);
	vcamNote_->setWordWrap(true);
	vcamNote_->setText(T("VirtualCam.NotVerified"));
	vcamNote_->setVisible(decisions::kVirtualCamBeta == decisions::VirtualCamBeta::Note);
	v->addWidget(vcamNote_);
	v->addStretch(1);
	connect(vcamButton_, &QPushButton::clicked, this, &Dock::virtualCamAction);
	return w;
}

QWidget *Dock::buildLines()
{
	auto *w = new QWidget(this);
	w->setObjectName(QStringLiteral("outputLines"));
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(4);

	const auto line = [&](QLabel *&iconOut) {
		QFrame *f = lineFrame(w);
		auto *h = new QHBoxLayout(f);
		h->setContentsMargins(8, 0, 2, 0);
		h->setSpacing(8);
		iconOut = iconLabel(f, tokens::kIconSmall);
		h->addWidget(iconOut);
		v->addWidget(f);
		return std::make_pair(f, h);
	};

	auto [recF, recH] = line(recLineIcon_);
	recLine_ = new ElidedLabel(recF);
	recLine_->setObjectName(QStringLiteral("recLine"));
	recLineButton_ = new QToolButton(recF);
	recLineButton_->setObjectName(QStringLiteral("recLineButton"));
	recH->addWidget(recLine_, 1);
	recH->addWidget(recLineButton_);
	connect(recLineButton_, &QToolButton::clicked, this, &Dock::toggleRecording);

	QLabel *replayIcon = nullptr;
	auto [repF, repH] = line(replayIcon);
	replayIcon->setObjectName(QStringLiteral("replayLineIcon"));
	replayLine_ = new ElidedLabel(repF);
	replayLine_->setObjectName(QStringLiteral("replayLine"));
	replayLineSave_ = new QToolButton(repF);
	replayLineSave_->setText(T("Dock.SaveShort"));
	replayLineSave_->setToolTip(T("Dock.SaveReplay"));
	replayLineSave_->setFixedHeight(tokens::kButton);
	replayLineButton_ = new QToolButton(repF);
	replayLineButton_->setObjectName(QStringLiteral("replayLineButton"));
	repH->addWidget(replayLine_, 1);
	repH->addWidget(replayLineSave_);
	repH->addWidget(replayLineButton_);
	connect(replayLineButton_, &QToolButton::clicked, this, &Dock::toggleReplay);
	connect(replayLineSave_, &QToolButton::clicked, this, [] { Outputs::instance().saveReplay(); });

	QLabel *camIcon = nullptr;
	auto [camF, camH] = line(camIcon);
	camIcon->setObjectName(QStringLiteral("vcamLineIcon"));
	vcamLine_ = camF;
	auto *camName = new ElidedLabel(camF);
	camName->setFullText(T("VirtualCam.Name"));
	camH->addWidget(camName, 1);
	if (decisions::kVirtualCamBeta == decisions::VirtualCamBeta::Badge)
		camH->addWidget(new BetaBadge(camF));
	vcamLineDot_ = new StatusDot(camF);
	camH->addWidget(vcamLineDot_);
	vcamLineButton_ = new QToolButton(camF);
	vcamLineButton_->setObjectName(QStringLiteral("vcamLineButton"));
	camH->addWidget(vcamLineButton_);
	connect(vcamLineButton_, &QToolButton::clicked, this, &Dock::virtualCamAction);
#ifndef VERTICAL_VIRTUALCAM
	vcamLine_->hide();
#endif
	return w;
}

QWidget *Dock::buildFooter()
{
	auto *w = new QWidget(this);
	w->setObjectName(QStringLiteral("dockFooter"));
	auto *outer = new QVBoxLayout(w);
	outer->setContentsMargins(tokens::kPadding, 7, tokens::kPadding, 7);
	outer->setSpacing(4);
	auto *h = new QHBoxLayout();
	h->setSpacing(tokens::kGap);
	encoderLabel_ = mutedSmall(w);
	encoderLabel_->setText(T("Dock.SharedEncoder"));
	encoderChip_ = new Chip(w);
	encoderChip_->setObjectName(QStringLiteral("encoderChip"));
	encoderChip_->setFont(scaledFont(font(), 10.5 / 12.0, true));
	outputsLive_ = mutedSmall(w);
	outputsLive_->setObjectName(QStringLiteral("footerOutputs"));
	cpu_ = mutedSmall(w);
	cpu_->setObjectName(QStringLiteral("footerCpu"));
	version_ = mutedSmall(w);
	version_->setText(QStringLiteral("Vertical for OBS %1").arg(QString::fromUtf8(PLUGIN_VERSION)));
	gear_ = iconButton("gear", T("Dock.Settings"), tokens::kSmallButton, true, w);
	gear_->setObjectName(QStringLiteral("footerSettings"));
	connect(gear_, &QToolButton::clicked, this, [this] { openSettings(); });
	/* The status line may clip rather than widen the dock. */
	for (QLabel *l : {encoderLabel_, outputsLive_, cpu_, version_})
		l->setMinimumWidth(1);
	h->addWidget(encoderLabel_);
	h->addWidget(encoderChip_);
	h->addWidget(outputsLive_);
	h->addWidget(cpu_);
	h->addStretch(1);
	outer->addLayout(h);
	/* Compact: outputs and CPU wrap onto a second line. */
	footerWrap_ = new QWidget(w);
	auto *wrap = new QHBoxLayout(footerWrap_);
	wrap->setContentsMargins(0, 0, 0, 0);
	wrap->setSpacing(tokens::kGap);
	footerWrapLayout_ = wrap;
	footerRowLayout_ = h;
	outer->addWidget(footerWrap_);
	footerWrap_->hide();
	if (decisions::kFooter == decisions::Footer::TwoLines) {
		footerSecond_ = new QWidget(w);
		auto *h2 = new QHBoxLayout(footerSecond_);
		h2->setContentsMargins(0, 0, 0, 0);
		h2->addWidget(version_);
		h2->addStretch(1);
		h2->addWidget(gear_);
		outer->addWidget(footerSecond_);
	} else {
		h->addWidget(version_);
		h->addWidget(gear_);
	}
	return w;
}

/* ================================================================ layout */

void Dock::setLayoutChoice(core::LayoutChoice choice)
{
	if (decisions::kModes == decisions::Modes::Automatic)
		choice = core::LayoutChoice::Automatic;
	choice_ = choice;
	if (config_t *cfg = obs_frontend_get_user_config())
		config_set_string(cfg, kConfigSection, kConfigLayout, core::layoutChoiceId(choice));
	updateMode();
}

void Dock::updateMode()
{
	const core::DockMode m = core::chooseDockMode(width(), choice_, mode_);
	if (m != mode_)
		applyMode(m);
	/* Narrow Studio: Start all / Stop all keep their icon only. */
	const bool studio = mode_ == core::DockMode::Studio;
	const bool narrow = studio && width() - tokens::kStudioLeftColumn - 1 - 2 * tokens::kPadding < kNarrowOutputs;
	startAll_->setText(narrow ? QString() : T("Dock.StartAll"));
	stopAll_->setText(narrow ? QString() : T("Dock.StopAll"));
	/* Header and status line drop their least useful text first. */
	linkedLabel_->setVisible(width() >= kHeaderLabelMin);
	encoderLabel_->setVisible(studio && width() >= kFooterEncoderLabelMin);
	version_->setVisible((studio || decisions::kFooter == decisions::Footer::TwoLines) &&
			     (width() >= kFooterVersionMin || decisions::kFooter == decisions::Footer::TwoLines));
}

void Dock::applyMode(core::DockMode mode)
{
	mode_ = mode;
	const bool studio = mode == core::DockMode::Studio;
	const bool tabs = studio && decisions::kRecording == decisions::Recording::TabsInStudio;

	for (QVBoxLayout *l : {colALayout_, colBLayout_})
		while (QLayoutItem *it = l->takeAt(0))
			delete it;

	int maxPreview = tokens::kPreviewMaxCompact;
	if (studio)
		maxPreview = decisions::kPreviewSize == decisions::PreviewSize::Max360   ? tokens::kPreviewMaxStudio
			     : decisions::kPreviewSize == decisions::PreviewSize::Max480 ? tokens::kPreviewMax480
											 : 0;
	preview_->setMaxPreviewHeight(maxPreview);

	colALayout_->addWidget(preview_);
	if (studio) {
		colA_->setFixedWidth(tokens::kStudioLeftColumn);
		colALayout_->addWidget(scenesHeader_);
		colALayout_->addWidget(scenes_, 1);
		colBLayout_->addWidget(outputsHeader_);
		colBLayout_->addWidget(destinationsBox_);
		colBLayout_->addWidget(tabs ? tabsBox_ : linesBox_);
		colBLayout_->addStretch(1);
	} else {
		colA_->setMinimumWidth(0);
		colA_->setMaximumWidth(QWIDGETSIZE_MAX);
		colALayout_->addWidget(scenes_, 1);
		colALayout_->addWidget(destinationsBox_);
		colALayout_->addWidget(linesBox_);
	}
	preview_->show();
	scenes_->show();
	destinationsBox_->show();
	scenesHeader_->setVisible(studio);
	outputsHeader_->setVisible(studio);
	tabsBox_->setVisible(tabs);
	linesBox_->setVisible(!tabs);
	colB_->setVisible(studio);
	divider_->setVisible(studio);
	scenes_->setRowHeight(studio ? tokens::kRowStudio : tokens::kRowCompact);

	footer_->setVisible(studio || decisions::kFooter != decisions::Footer::HiddenInCompact);
	/* Status line: one line in Studio; in Compact outputs and CPU go to a second line. */
	footerRowLayout_->removeWidget(outputsLive_);
	footerRowLayout_->removeWidget(cpu_);
	while (QLayoutItem *it = footerWrapLayout_->takeAt(0))
		delete it;
	if (studio) {
		footerRowLayout_->insertWidget(2, outputsLive_);
		footerRowLayout_->insertWidget(3, cpu_);
	} else {
		footerWrapLayout_->addWidget(outputsLive_);
		footerWrapLayout_->addWidget(cpu_);
		footerWrapLayout_->addStretch(1);
	}
	footerWrap_->setVisible(!studio);
	outputsLive_->show();
	cpu_->show();

	rebuildDestinations();
	updateMode();
	setProperty("verticalMode", studio ? QStringLiteral("studio") : QStringLiteral("compact"));
}

void Dock::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	updateMode();
}

void Dock::changeEvent(QEvent *e)
{
	QWidget::changeEvent(e);
	if (e->type() == QEvent::ApplicationPaletteChange)
		applyColors();
}

void Dock::applyColors()
{
	const Surfaces s = surfaces(this);
	for (QFrame *f : findChildren<QFrame *>(QStringLiteral("dockLine")))
		setSheet(f, QStringLiteral("background:%1;border:none;").arg(s.line.name()));
	for (QFrame *f : findChildren<QFrame *>(QStringLiteral("dockLineBox")))
		setSheet(f, QStringLiteral("QFrame#dockLineBox{background:%1;border:1px solid %2;border-radius:5px;}")
				    .arg(s.base.name(), s.line.name()));
	for (QLabel *l : findChildren<QLabel *>(QStringLiteral("muted")))
		setMuted(l, s.muted);
	setSheet(destinationsBox_,
		 QStringLiteral("QFrame#destinations{background:%1;border:1px solid %2;border-radius:%3px;}")
			 .arg(s.base.name(), s.line.name())
			 .arg(tokens::kListRadius));
	setSheet(destinationsScroll_, QStringLiteral("QScrollArea{background:transparent;}"));
	if (QFrame *bar = findChild<QFrame *>(QStringLiteral("tabsBar")))
		setSheet(bar, QStringLiteral("QFrame#tabsBar{background:%1;border:1px solid %2;border-radius:6px;}")
				      .arg(s.base.name(), s.line.name()));
	canvasChip_->setColors(s.row, s.line, s.muted, 4);
	setSheet(recordTimer_,
		 QStringLiteral("QLabel{background:%1;border:1px solid %2;border-radius:6px;padding:0 12px;}")
			 .arg(s.row.name(), s.line.name()));
	saveReplay_->setFixedHeight(tokens::kBigButton);
	setSheet(saveReplay_, QStringLiteral("QPushButton{background:%1;color:%2;border:1px solid %3;"
					     "border-radius:6px;padding:0 12px;}QPushButton:disabled{color:%4;}")
				      .arg(s.row.name(), s.text.name(), s.line.name(), s.muted.name()));
	setSheet(replayLineSave_, QStringLiteral("QToolButton{background:%1;color:%2;border:1px solid %3;"
						 "border-radius:5px;padding:0 8px;}")
					  .arg(s.row.name(), s.text.name(), s.line.name()));
	for (QPushButton *b : {startAll_, stopAll_})
		setSheet(b, QStringLiteral("QPushButton{background:%1;color:%2;border:1px solid %3;"
					   "border-radius:5px;padding:0 10px;}QPushButton:hover{border-color:%4;}"
					   "QPushButton:disabled{color:%4;}")
				    .arg(s.row.name(), s.text.name(), s.line.name(), s.muted.name()));
	startAll_->setIcon(icon("play", s.text, 11));
	stopAll_->setIcon(icon("stop", s.text, 11));
	phoneIcon_->setPixmap(iconPixmap("phone", s.muted, 14, devicePixelRatioF()));
	if (QLabel *l = findChild<QLabel *>(QStringLiteral("replayLineIcon")))
		l->setPixmap(iconPixmap("replay", s.muted, tokens::kIconSmall, devicePixelRatioF()));
	if (QLabel *l = findChild<QLabel *>(QStringLiteral("vcamLineIcon")))
		l->setPixmap(iconPixmap("cam", s.muted, tokens::kIconSmall, devicePixelRatioF()));
	restyleIconButton(more_, "more", true);
	restyleIconButton(gear_, "gear", true);
	restyleIconButton(addSceneButton_, "plus", false);
	scenes_->restyle();
	refreshStatus();
}

/* ================================================================ content */

void Dock::refreshHeader()
{
	Vertical &v = Vertical::instance();
	const core::Size s = v.canvasSize();
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	canvasChip_->setText(qs(core::canvasChip(s.width, s.height, ovi.fps_num, ovi.fps_den)));
	QString tip = T("Dock.CanvasTip");
	if (!v.canvas())
		tip += QStringLiteral(" · ") + T("Dock.NoCanvas");
	else if (!v.rendering())
		tip += QStringLiteral(" · ") + T("Dock.Idle");
	canvasChip_->setToolTip(tip);
	preview_->setAspect(s.width, s.height);
	refreshStatus();
}

void Dock::rebuildScenes()
{
	rebuilding_ = true;
	Vertical &v = Vertical::instance();
	const QString current = v.currentScene();
	const auto &links = v.settings().links;
	scenes_->clear();
	const QStringList names = v.sceneNames();
	for (const QString &name : names) {
		auto *item = new QListWidgetItem(name, scenes_);
		item->setData(SceneList::NameRole, name);
		item->setData(SceneList::ProgramRole, name == current);
		const auto mains = links.mainScenesFor(name.toStdString());
		item->setData(SceneList::LinkedRole, !mains.empty());
		QStringList m;
		for (const std::string &s : mains)
			m << qs(s);
		item->setToolTip(mains.empty() ? T("Link.Tip") : T("Link.LinkedTo").arg(m.join(QStringLiteral(", "))));
		if (name == current)
			scenes_->setCurrentItem(item);
	}
	scenesCount_->setText(QString::number(names.size()));
	{
		const QSignalBlocker block(linked_);
		linked_->setChecked(v.linkedMode());
	}
	rebuilding_ = false;
}

void Dock::rebuildDestinations()
{
	for (DestinationRow *r : rows_)
		r->deleteLater();
	rows_.clear();
	while (QLayoutItem *it = destinationsLayout_->takeAt(0))
		delete it;
	const bool compact = mode_ == core::DockMode::Compact;
	const auto &dests = Vertical::instance().settings().destinations;
	for (const core::Destination &d : dests) {
		auto *row = new DestinationRow(d.id, d.platform, qs(d.name), compact, destinationsList_);
		row->setSeparator(!rows_.empty());
		connect(row, &DestinationRow::toggleRequested, this,
			[](const std::string &id) { Outputs::instance().toggleDestination(id); });
		connect(row, &DestinationRow::editRequested, this, &Dock::editDestination);
		connect(row, &DestinationRow::duplicateRequested, this, &Dock::duplicateDestination);
		connect(row, &DestinationRow::deleteRequested, this, &Dock::deleteDestination);
		destinationsLayout_->addWidget(row);
		rows_.push_back(row);
	}
	destinationsLayout_->addStretch(1);
	noDestinations_->setVisible(dests.empty());
	destinationsScroll_->setVisible(!dests.empty());
	/* The list shows its rows (QScrollArea caches the size of its content, so the
	 * height is set here), then scrolls beyond a few rows. */
	int rowsH = 0;
	const int cap = compact ? tokens::kCompactDestinationsMax : tokens::kStudioDestinationsMax;
	for (size_t i = 0; i < rows_.size() && (int)i < cap; i++)
		rowsH += rows_[i]->sizeHint().height();
	destinationsScroll_->setFixedHeight(std::max(rowsH, 1));
	refreshStatus();
}

core::StatusTexts Dock::statusTexts() const
{
	core::StatusTexts t;
	t.live = T("Status.Live").toStdString();
	t.connecting = T("Status.Starting").toStdString();
	t.reconnecting = T("Status.ReconnectingCount").toStdString();
	t.reconnectingNoCount = T("Status.Reconnecting").toStdString();
	t.stopping = T("Status.Stopping").toStdString();
	t.stopped = T("Status.Stopped").toStdString();
	t.error = T("Status.ErrorDetail").toStdString();
	t.errorNoDetail = T("Status.Error").toStdString();
	t.rate = T("Status.Rate").toStdString();
	t.dropped = T("Status.Dropped").toStdString();
	const QString dec = T("Status.Decimal");
	t.decimal = dec.isEmpty() ? '.' : dec.at(0).toLatin1();
	return t;
}

core::StatusView Dock::viewFor(const std::string &id, bool) const
{
	const OutputStatus st = Outputs::instance().destinationStatus(id);
	core::StatusInput in;
	in.phase = phaseOf(st.state);
	in.elapsedSec = core::elapsedSeconds(st.startedNs, os_gettime_ns());
	in.kbps = st.kbps;
	in.dropped = st.dropped;
	in.attempt = st.attempt;
	in.maxAttempts = Outputs::kReconnectRetries;
	in.error = st.error.toStdString();
	return core::describeStatus(in, statusTexts());
}

core::VcamState Dock::vcamState() const
{
#ifdef VERTICAL_VIRTUALCAM
	const bool supported = true;
#else
	const bool supported = false;
#endif
	return core::vcamState(supported, vcamInstalled_, vcamInstallRequested_,
			       Outputs::instance().virtualCamActive());
}

void Dock::refreshStatus()
{
	if (!startAll_)
		return;
	Outputs &o = Outputs::instance();
	const core::Settings &settings = Vertical::instance().settings();
	const Surfaces s = surfaces(this);

	/* Destinations. */
	bool anyStartable = false;
	for (DestinationRow *row : rows_) {
		const core::Destination *d = settings.findDestination(row->id());
		const bool enabled = d && d->enabled;
		const core::StatusView view = viewFor(row->id(), true);
		row->setStatus(view, enabled);
		if (enabled && !view.running)
			anyStartable = true;
	}
	const int live = o.liveStreamCount();
	outputsCount_->setText(T("Dock.LiveCount").arg(live));
	startAll_->setEnabled(anyStartable);
	stopAll_->setEnabled(o.anyStreamActive());

	/* Preview: LIVE pill and red frame while a destination is live. */
	const core::Size cs = Vertical::instance().canvasSize();
	const QString tag = qs(core::aspectTag(cs.width, cs.height));
	preview_->setLive(live > 0);
	preview_->preview()->setOverlay(live > 0, tag, true);
	if (previewWindow_)
		previewWindow_->preview()->setOverlay(live > 0, tag, false);

	/* Recording. */
	const OutputStatus rec = o.recordingStatus();
	const bool recording = o.recordingActive();
	const QString clock = qs(core::formatClock(core::elapsedSeconds(rec.startedNs, os_gettime_ns())));
	recordButton_->setText(recording ? T("Dock.StopRecording") : T("Dock.StartRecording"));
	styleBigButton(recordButton_, recording, "rec", tokens::kRed);
	recordTimer_->setText(recording ? clock : QStringLiteral("00:00:00"));
	{
		QPalette p = recordTimer_->palette();
		p.setColor(QPalette::WindowText, recording ? s.text : s.muted);
		recordTimer_->setPalette(p);
	}
	const bool recError = rec.state == OutputState::Error && !rec.error.isEmpty();
	recordError_->setVisible(recError);
	if (recError) {
		recordError_->setFullText(T("Status.ErrorDetail").arg(rec.error));
		QPalette p = recordError_->palette();
		p.setColor(QPalette::WindowText, QColor::fromRgba(tokens::kErrorText));
		recordError_->setPalette(p);
	}
	const core::RecordingSettings &rs = settings.recording;
	const core::Size out = Vertical::instance().outputSize();
	recordDetail_->setFullText(
		QStringLiteral("%1 · %2 · %3")
			.arg(T(QStringLiteral("Recording.Short.") + QString::fromUtf8(core::recordFormatId(rs.format))))
			.arg(qs(core::canvasChip(out.width, out.height, 0, 0)))
			.arg(T("Status.Rate")
				     .arg(qs(core::formatMbps(rs.encoding.video.bitrateKbps, statusTexts().decimal)))));
	recLine_->setFullText(recording ? T("Dock.RecLine").arg(clock) : T("Dock.RecLineIdle"));
	recLineIcon_->setPixmap(iconPixmap("rec", recording ? QColor::fromRgba(tokens::kRed) : s.muted,
					   tokens::kIconSmall, devicePixelRatioF()));
	styleStartStop(recLineButton_, recording);
	recLineButton_->setToolTip(recording ? T("Dock.StopRecording") : T("Dock.StartRecording"));

	/* Replay buffer. */
	const bool replay = o.replayActive();
	replayButton_->setText(replay ? T("Dock.StopReplay") : T("Dock.StartReplay"));
	styleBigButton(replayButton_, replay, "replay", 0);
	saveReplay_->setEnabled(replay);
	replayDetail_->setFullText(T("Replay.Detail").arg(settings.replay.seconds).arg(settings.replay.maxMb));
	replayLine_->setFullText(replay ? T("Dock.ReplayLineOn") : T("Dock.ReplayLineIdle"));
	replayLineSave_->setVisible(replay);
	styleStartStop(replayLineButton_, replay);
	replayLineButton_->setToolTip(replay ? T("Dock.StopReplay") : T("Dock.StartReplay"));

	refreshVirtualCam();
	refreshFooter();
}

void Dock::refreshVirtualCam()
{
	const core::VcamState st = vcamState();
	core::Tone tone = core::Tone::Idle;
	QString text, action;
	bool running = false;
	switch (st) {
	case core::VcamState::NotInstalled:
		text = T("VirtualCam.State.NotInstalled");
		action = T("VirtualCam.Action.Install");
		break;
	case core::VcamState::WaitingApproval:
		tone = core::Tone::Busy;
		text = T("VirtualCam.State.Waiting");
		action = T("VirtualCam.Action.OpenSettings");
		break;
	case core::VcamState::Activated:
		tone = core::Tone::Ok;
		text = T("VirtualCam.State.Activated");
		action = T("VirtualCam.Action.Start");
		break;
	case core::VcamState::Running: {
		tone = core::Tone::Live;
		obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		text = T("VirtualCam.State.Running")
			       .arg(qs(core::canvasChip(virtualcam::kDefaultWidth, virtualcam::kDefaultHeight,
							ovi.fps_num, ovi.fps_den)));
		action = T("VirtualCam.Action.Stop");
		running = true;
		break;
	}
	default:
		text = T("VirtualCam.State.Unavailable");
		break;
	}
	vcamDot_->setTone(tone, false);
	vcamStatus_->setFullText(text);
	setMuted(vcamStatus_, surfaces(this).muted);
	vcamButton_->setText(action);
	vcamButton_->setVisible(!action.isEmpty());
	styleBigButton(vcamButton_, running, "cam", 0);
	vcamLineDot_->setTone(tone, false);
	vcamLineDot_->setToolTip(text);
	styleStartStop(vcamLineButton_, running);
	vcamLineButton_->setToolTip(action);
}

void Dock::refreshFooter()
{
	Outputs &o = Outputs::instance();
	const EncoderChip c = o.encoderChip();
	if (c.none) {
		encoderChip_->setText(T("Encoder.NoneShort"));
		encoderChip_->setColors(QColor::fromRgba(tokens::kChipWarnFill),
					QColor::fromRgba(tokens::kChipWarnBorder),
					QColor::fromRgba(tokens::kChipWarnText), 10);
	} else {
		encoderChip_->setText(T(c.hardware ? "Encoder.ChipHardware" : "Encoder.ChipSoftware").arg(c.family));
		if (c.hardware)
			encoderChip_->setColors(QColor::fromRgba(tokens::kChipOkFill),
						QColor::fromRgba(tokens::kChipOkBorder),
						QColor::fromRgba(tokens::kChipOkText), 10);
		else
			encoderChip_->setColors(QColor::fromRgba(tokens::kChipWarnFill),
						QColor::fromRgba(tokens::kChipWarnBorder),
						QColor::fromRgba(tokens::kChipWarnText), 10);
	}
	encoderChip_->setToolTip(o.encoderSummary());
	const Surfaces s = surfaces(this);
	const QString bold = QStringLiteral("<b style='color:%1'>%2</b>").arg(s.text.name());
	outputsLive_->setText(T("Dock.FooterOutputs").arg(bold.arg(T("Dock.LiveCount").arg(o.liveStreamCount()))));
	QString cpu = QStringLiteral("–");
	if (cpuValue_ >= 0.0) {
		cpu = QString::number(cpuValue_, 'f', 1);
		cpu.replace(QLatin1Char('.'), QChar::fromLatin1(statusTexts().decimal));
		cpu += QStringLiteral(" %");
	}
	cpu_->setText(T("Dock.FooterCpu").arg(bold.arg(cpu)));
}

void Dock::refreshTimers()
{
	if (cpuInfo_)
		cpuValue_ = os_cpu_usage_info_query(cpuInfo_);
#ifdef VERTICAL_VIRTUALCAM
	if (!Outputs::instance().virtualCamActive() && ++vcamPollTicks_ >= kVirtualCamPollSeconds) {
		vcamPollTicks_ = 0;
		const bool installed = virtualcam::isExtensionInstalled();
		if (installed)
			vcamInstallRequested_ = false;
		vcamInstalled_ = installed;
	}
#endif
	if (isVisible())
		refreshStatus();
}

/* ================================================================ preview window */

PreviewWindow *Dock::previewWindow() const
{
	return previewWindow_.data();
}

void Dock::openPreviewWindow()
{
	if (previewWindow_) {
		previewWindow_->show();
		previewWindow_->raise();
		previewWindow_->activateWindow();
		return;
	}
	previewWindow_ = new PreviewWindow(this);
	previewWindow_->preview()->setCanvas(Vertical::instance().canvas());
	const core::Size cs = Vertical::instance().canvasSize();
	previewWindow_->preview()->setOverlay(Outputs::instance().liveStreamCount() > 0,
					      qs(core::aspectTag(cs.width, cs.height)), false);
	previewWindow_->show();
}

/* ================================================================ link menu */

QMenu *Dock::openLinkMenu(const QString &verticalScene, const QPoint &globalPos)
{
	auto *menu = new StickyMenu(this);
	menu->setObjectName(QStringLiteral("verticalLinkMenu"));
	menu->setAttribute(Qt::WA_DeleteOnClose);
	QAction *title = menu->addAction(T("Link.MenuTitle").arg(verticalScene));
	title->setEnabled(false);

	const std::vector<std::string> mains = mainSceneNames();
	if (mains.empty())
		menu->addAction(T("Link.NoMainScenes"))->setEnabled(false);
	const auto entries =
		core::linkMenuEntries(Vertical::instance().settings().links, mains, verticalScene.toStdString());
	for (const core::LinkMenuEntry &e : entries) {
		const QString main = qs(e.mainScene);
		QAction *a = menu->addAction(e.linkedTo.empty() ? main : T("Link.MovesFrom").arg(main, qs(e.linkedTo)));
		a->setCheckable(true);
		a->setChecked(e.checked);
		a->setData(main);
		connect(a, &QAction::triggered, menu, [a, main, verticalScene](bool checked) {
			Vertical &v = Vertical::instance();
			const auto now = core::linkMenuEntries(v.settings().links, {main.toStdString()},
							       verticalScene.toStdString());
			switch (core::linkActionFor(now.front(), checked)) {
			case core::LinkAction::Link:
				v.linkScene(main, verticalScene);
				break;
			case core::LinkAction::Unlink:
				v.unlinkScene(main);
				break;
			default:
				break;
			}
			const auto after = core::linkMenuEntries(v.settings().links, {main.toStdString()},
								 verticalScene.toStdString());
			const QSignalBlocker block(a);
			a->setChecked(after.front().checked);
			a->setText(after.front().linkedTo.empty()
					   ? main
					   : T("Link.MovesFrom").arg(main, qs(after.front().linkedTo)));
		});
	}
	menu->addSeparator();
	QAction *unlinkAll = menu->addAction(T("Link.UnlinkAll"));
	unlinkAll->setObjectName(QStringLiteral("unlinkAll"));
	connect(unlinkAll, &QAction::triggered, this, [verticalScene] {
		Vertical &v = Vertical::instance();
		for (const std::string &m : v.settings().links.mainScenesFor(verticalScene.toStdString()))
			v.unlinkScene(qs(m));
	});
	unlinkAll->setEnabled(
		!Vertical::instance().settings().links.mainScenesFor(verticalScene.toStdString()).empty());
	menu->popup(globalPos);
	return menu;
}

/* ================================================================ actions */

void Dock::onSceneMenu(const QPoint &pos)
{
	QMenu menu(this);
	menu.addAction(T("Scene.Add"), this, &Dock::addScene);
	if (QListWidgetItem *item = scenes_->itemAt(pos)) {
		scenes_->setCurrentItem(item);
		const QString name = item->data(SceneList::NameRole).toString();
		menu.addAction(T("Scene.Rename"), this, &Dock::renameScene);
		menu.addAction(T("Scene.Remove"), this, &Dock::removeScene);
		menu.addSeparator();
		const QPoint global = scenes_->viewport()->mapToGlobal(pos);
		menu.addAction(T("Link.Open"), this, [this, name, global] { openLinkMenu(name, global); });
	}
	menu.exec(scenes_->viewport()->mapToGlobal(pos));
}

void Dock::addScene()
{
	bool ok = false;
	const QString name = QInputDialog::getText(this, T("Scene.Add"), T("Scene.NamePrompt"), QLineEdit::Normal,
						   T("Scene.DefaultName"), &ok);
	if (ok)
		Vertical::instance().addScene(name);
}

void Dock::removeScene()
{
	QListWidgetItem *item = scenes_->currentItem();
	if (!item)
		return;
	const QString name = item->data(SceneList::NameRole).toString();
	if (Vertical::instance().sceneNames().size() <= 1) {
		showError(T("Scene.Remove"), T("Scene.KeepOne"));
		return;
	}
	if (QMessageBox::question(this, T("Scene.Remove"), T("Scene.RemoveConfirm").arg(name)) != QMessageBox::Yes)
		return;
	Vertical::instance().removeScene(name);
}

void Dock::renameScene()
{
	QListWidgetItem *item = scenes_->currentItem();
	if (!item)
		return;
	const QString from = item->data(SceneList::NameRole).toString();
	bool ok = false;
	const QString to =
		QInputDialog::getText(this, T("Scene.Rename"), T("Scene.NamePrompt"), QLineEdit::Normal, from, &ok);
	if (!ok)
		return;
	QString error;
	if (!Vertical::instance().renameScene(from, to, &error) && !error.isEmpty())
		showError(T("Scene.Rename"), error);
}

void Dock::toggleRecording()
{
	Outputs &out = Outputs::instance();
	if (out.recordingActive()) {
		out.stopRecording();
		return;
	}
	QString error;
	if (!out.startRecording(&error))
		showError(T("Dock.Tab.Recording"), error);
}

void Dock::toggleReplay()
{
	Outputs &out = Outputs::instance();
	if (out.replayActive()) {
		out.stopReplay();
		return;
	}
	QString error;
	if (!out.startReplay(&error))
		showError(T("Dock.Replay"), error);
}

void Dock::virtualCamAction()
{
	Outputs &out = Outputs::instance();
	switch (vcamState()) {
	case core::VcamState::Running:
		out.stopVirtualCam();
		break;
	case core::VcamState::Activated: {
		QString error;
		if (!out.startVirtualCam(&error))
			showError(T("VirtualCam.Name"), error);
		break;
	}
	case core::VcamState::WaitingApproval:
		QDesktopServices::openUrl(
			QUrl(QStringLiteral("x-apple.systempreferences:com.apple.LoginItems-Settings.extension")));
		break;
	case core::VcamState::NotInstalled:
		if (virtualcam::isHostAppPresent() && virtualcam::openHostApp(true))
			vcamInstallRequested_ = true;
		else
			showError(T("VirtualCam.Name"), T("VirtualCam.Error.NotInstalled"));
		break;
	default:
		break;
	}
	refreshStatus();
}

void Dock::addDestination(core::Platform p)
{
	core::Settings &s = Vertical::instance().settings();
	const std::string id =
		core::makeId((unsigned long long)QDateTime::currentMSecsSinceEpoch(), s.destinationIds());
	DestinationDialog dlg(core::makeDestination(p, id, core::platformInfo(p).displayName), this);
	if (dlg.exec() != QDialog::Accepted)
		return;
	s.destinations.push_back(dlg.destination());
	Vertical::instance().settingsEdited();
	hotkeys::syncDestinations();
}

void Dock::editDestination(const std::string &id)
{
	core::Settings &s = Vertical::instance().settings();
	const core::Destination *d = s.findDestination(id);
	if (!d)
		return;
	DestinationDialog dlg(*d, this);
	if (dlg.exec() != QDialog::Accepted)
		return;
	if (core::Destination *target = s.findDestination(id))
		*target = dlg.destination();
	Vertical::instance().settingsEdited();
	hotkeys::syncDestinations();
}

void Dock::duplicateDestination(const std::string &id)
{
	core::Settings &s = Vertical::instance().settings();
	const core::Destination *d = s.findDestination(id);
	if (!d)
		return;
	core::Destination copy = *d;
	copy.id = core::makeId((unsigned long long)QDateTime::currentMSecsSinceEpoch(), s.destinationIds());
	copy.name += " 2";
	copy.streamKey.clear(); /* two outputs never push to the same key */
	s.destinations.push_back(copy);
	Vertical::instance().settingsEdited();
	hotkeys::syncDestinations();
}

void Dock::deleteDestination(const std::string &id)
{
	core::Settings &s = Vertical::instance().settings();
	const core::Destination *d = s.findDestination(id);
	if (!d)
		return;
	const OutputState st = Outputs::instance().destinationStatus(id).state;
	if (st != OutputState::Idle && st != OutputState::Error) {
		showError(qs(d->name), T("Destination.BusyRemove"));
		return;
	}
	if (QMessageBox::question(this, T("Destination.Delete"), T("Destination.DeleteConfirm").arg(qs(d->name))) !=
	    QMessageBox::Yes)
		return;
	auto &list = s.destinations;
	list.erase(std::remove_if(list.begin(), list.end(), [&](const core::Destination &x) { return x.id == id; }),
		   list.end());
	Vertical::instance().settingsEdited();
	hotkeys::syncDestinations();
}

void Dock::openSettings(int tab)
{
	SettingsDialog dlg(this, (SettingsDialog::Tab)tab);
	dlg.exec();
}

void Dock::showError(const QString &title, const QString &message)
{
	QMessageBox::warning(this, title.isEmpty() ? T("Dock.Title") : title, message);
}

} // namespace vertical::ui
