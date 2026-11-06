#include "ui/dock.hpp"

#include "obs/outputs.hpp"
#include "obs/text.hpp"
#include "obs/vertical.hpp"
#ifdef VERTICAL_VIRTUALCAM
#include "obs/virtualcam.hpp"
#endif
#include "ui/preview.hpp"
#include "ui/settings-dialog.hpp"

#include <util/platform.h>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

namespace vertical::ui {

namespace {

/* Status colours (to be validated in the design review). */
const char *kIdle = "#6b7280";
const char *kBusy = "#e3a748";
const char *kLive = "#e5484d";

QString elapsed(uint64_t startedNs)
{
	if (!startedNs)
		return QString();
	const uint64_t s = (os_gettime_ns() - startedNs) / 1000000000ull;
	return QStringLiteral("%1:%2:%3")
		.arg(s / 3600)
		.arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
		.arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString dotStyle(OutputState st)
{
	const char *c = kIdle;
	bool ring = false;
	switch (st) {
	case OutputState::Starting:
	case OutputState::Reconnecting:
	case OutputState::Stopping:
		c = kBusy;
		break;
	case OutputState::Live:
		c = kLive;
		break;
	case OutputState::Error:
		c = kLive;
		ring = true;
		break;
	default:
		break;
	}
	return ring ? QStringLiteral("border:2px solid %1;border-radius:5px;background:transparent;").arg(c)
		    : QStringLiteral("border-radius:5px;background:%1;").arg(c);
}

QString stateText(const OutputStatus &st)
{
	switch (st.state) {
	case OutputState::Starting:
		return T("Status.Starting");
	case OutputState::Reconnecting:
		return T("Status.Reconnecting");
	case OutputState::Stopping:
		return T("Status.Stopping");
	case OutputState::Live:
		return QStringLiteral("%1 · %2 kbps").arg(elapsed(st.startedNs)).arg((int)st.kbps) +
		       (st.dropped ? QStringLiteral(" · ") + T("Status.Dropped").arg(st.dropped) : QString());
	case OutputState::Error:
		return st.error.isEmpty() ? T("Status.Error") : st.error;
	default:
		return T("Status.Idle");
	}
}

QLabel *sectionLabel(const QString &text, QWidget *parent)
{
	auto *l = new QLabel(text.toUpper(), parent);
	QFont f = l->font();
	f.setBold(true);
	f.setPointSizeF(f.pointSizeF() * 0.85);
	l->setFont(f);
	return l;
}

QToolButton *toolButton(const QString &text, const QString &tip, QWidget *parent)
{
	auto *b = new QToolButton(parent);
	b->setText(text);
	b->setToolTip(tip);
	b->setAccessibleName(tip);
	b->setMinimumSize(28, 28);
	return b;
}

} // namespace

Dock::Dock(QWidget *parent) : QWidget(parent)
{
	Vertical &v = Vertical::instance();
	Outputs &o = Outputs::instance();

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	root->setSpacing(6);

	/* Header: canvas size, preview toggle, settings. */
	auto *header = new QHBoxLayout();
	canvasBadge_ = new QLabel(this);
	previewToggle_ = toolButton(T("Dock.Preview"), T("Dock.PreviewTip"), this);
	previewToggle_->setCheckable(true);
	previewToggle_->setChecked(v.settings().previewVisible);
	auto *gear = toolButton(QStringLiteral("⚙"), T("Dock.Settings"), this);
	header->addWidget(canvasBadge_, 1);
	header->addWidget(previewToggle_);
	header->addWidget(gear);
	root->addLayout(header);

	auto *split = new QSplitter(Qt::Vertical, this);
	split->setChildrenCollapsible(false);

	preview_ = new Preview(split);
	preview_->setActive(v.settings().previewVisible);
	preview_->setVisible(v.settings().previewVisible);
	split->addWidget(preview_);

	/* Scenes. */
	auto *scenesBox = new QWidget(split);
	auto *scenesLayout = new QVBoxLayout(scenesBox);
	scenesLayout->setContentsMargins(0, 0, 0, 0);
	scenesLayout->addWidget(sectionLabel(T("Dock.Scenes"), scenesBox));
	scenes_ = new QListWidget(scenesBox);
	scenes_->setDragDropMode(QAbstractItemView::InternalMove);
	scenes_->setContextMenuPolicy(Qt::CustomContextMenu);
	scenesLayout->addWidget(scenes_, 1);
	auto *sceneBar = new QHBoxLayout();
	auto *add = toolButton(QStringLiteral("+"), T("Scene.Add"), scenesBox);
	auto *remove = toolButton(QStringLiteral("−"), T("Scene.Remove"), scenesBox);
	auto *rename = toolButton(QStringLiteral("✎"), T("Scene.Rename"), scenesBox);
	auto *up = toolButton(QStringLiteral("↑"), T("Scene.Up"), scenesBox);
	auto *down = toolButton(QStringLiteral("↓"), T("Scene.Down"), scenesBox);
	auto *link = toolButton(QStringLiteral("↔"), T("Scene.LinkCurrent"), scenesBox);
	for (QToolButton *b : {add, remove, rename, up, down, link})
		sceneBar->addWidget(b);
	sceneBar->addStretch();
	linked_ = new QCheckBox(T("Scene.LinkedMode"), scenesBox);
	linked_->setToolTip(T("Scene.LinkedModeTip"));
	linked_->setChecked(v.linkedMode());
	sceneBar->addWidget(linked_);
	scenesLayout->addLayout(sceneBar);
	split->addWidget(scenesBox);

	/* Outputs. */
	auto *outputsBox = new QWidget(split);
	auto *outputsLayout = new QVBoxLayout(outputsBox);
	outputsLayout->setContentsMargins(0, 0, 0, 0);
	outputsLayout->addWidget(sectionLabel(T("Dock.Outputs"), outputsBox));
	destinationsBox_ = new QWidget(outputsBox);
	destinationsLayout_ = new QVBoxLayout(destinationsBox_);
	destinationsLayout_->setContentsMargins(0, 0, 0, 0);
	destinationsLayout_->setSpacing(2);
	outputsLayout->addWidget(destinationsBox_);
	noDestinations_ = new QLabel(T("Dock.NoDestinations"), outputsBox);
	noDestinations_->setWordWrap(true);
	outputsLayout->addWidget(noDestinations_);

	auto *allRow = new QHBoxLayout();
	startAll_ = new QPushButton(T("Dock.StartAll"), outputsBox);
	stopAll_ = new QPushButton(T("Dock.StopAll"), outputsBox);
	allRow->addWidget(startAll_);
	allRow->addWidget(stopAll_);
	outputsLayout->addLayout(allRow);

	auto *recRow = new QHBoxLayout();
	record_ = new QPushButton(T("Dock.Record"), outputsBox);
	record_->setCheckable(true);
	recordTime_ = new QLabel(outputsBox);
	replay_ = new QPushButton(T("Dock.Replay"), outputsBox);
	replay_->setCheckable(true);
	saveReplay_ = new QPushButton(T("Dock.SaveReplay"), outputsBox);
	recRow->addWidget(record_);
	recRow->addWidget(recordTime_, 1);
	recRow->addWidget(replay_);
	recRow->addWidget(saveReplay_);
	outputsLayout->addLayout(recRow);

	virtualCam_ = new QPushButton(T("Dock.VirtualCam"), outputsBox);
	virtualCam_->setCheckable(true);
	outputsLayout->addWidget(virtualCam_);

	encoderInfo_ = new QLabel(outputsBox);
	encoderInfo_->setWordWrap(true);
	outputsLayout->addWidget(encoderInfo_);
	outputsLayout->addStretch();
	split->addWidget(outputsBox);
	split->setStretchFactor(0, 3);
	split->setStretchFactor(1, 2);
	split->setStretchFactor(2, 2);
	root->addWidget(split, 1);

	/* Wiring. */
	connect(previewToggle_, &QToolButton::toggled, this, [this](bool on) {
		Vertical::instance().settings().previewVisible = on;
		preview_->setVisible(on);
		preview_->setActive(on);
	});
	connect(gear, &QToolButton::clicked, this, &Dock::openSettings);
	connect(add, &QToolButton::clicked, this, &Dock::addScene);
	connect(remove, &QToolButton::clicked, this, &Dock::removeScene);
	connect(rename, &QToolButton::clicked, this, &Dock::renameScene);
	connect(up, &QToolButton::clicked, this, [this] { moveScene(-1); });
	connect(down, &QToolButton::clicked, this, [this] { moveScene(1); });
	connect(link, &QToolButton::clicked, this, &Dock::linkCurrentMainScene);
	connect(linked_, &QCheckBox::toggled, this, [](bool on) { Vertical::instance().setLinkedMode(on); });
	connect(scenes_, &QListWidget::itemClicked, this, [](QListWidgetItem *item) {
		Vertical::instance().setCurrentScene(item->data(Qt::UserRole).toString());
	});
	connect(scenes_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { renameScene(); });
	connect(scenes_, &QListWidget::customContextMenuRequested, this, &Dock::onSceneMenu);
	connect(scenes_->model(), &QAbstractItemModel::rowsMoved, this, [this] {
		if (rebuilding_)
			return;
		/* Drag and drop reorder: store the new order. */
		std::vector<std::string> order;
		for (int i = 0; i < scenes_->count(); i++)
			order.push_back(scenes_->item(i)->data(Qt::UserRole).toString().toStdString());
		Vertical::instance().settings().sceneOrder = order;
	});

	connect(startAll_, &QPushButton::clicked, this, [] { Outputs::instance().startAll(); });
	connect(stopAll_, &QPushButton::clicked, this, [] { Outputs::instance().stopAllStreams(); });
	connect(record_, &QPushButton::clicked, this, [this] {
		Outputs &out = Outputs::instance();
		if (out.recordingActive()) {
			out.stopRecording();
			return;
		}
		QString error;
		if (!out.startRecording(&error))
			showError(T("Dock.Record"), error);
	});
	connect(replay_, &QPushButton::clicked, this, [this] {
		Outputs &out = Outputs::instance();
		if (out.replayActive()) {
			out.stopReplay();
			return;
		}
		QString error;
		if (!out.startReplay(&error))
			showError(T("Dock.Replay"), error);
	});
	connect(saveReplay_, &QPushButton::clicked, this, [] { Outputs::instance().saveReplay(); });
	connect(virtualCam_, &QPushButton::clicked, this, [this] {
		Outputs &out = Outputs::instance();
		if (out.virtualCamActive()) {
			out.stopVirtualCam();
			return;
		}
		QString error;
		if (!out.startVirtualCam(&error)) {
#ifdef VERTICAL_VIRTUALCAM
			if (!virtualcam::isExtensionInstalled() && virtualcam::isHostAppPresent() &&
			    QMessageBox::question(this, T("Dock.VirtualCam"), T("VirtualCam.AskInstall")) ==
				    QMessageBox::Yes) {
				virtualcam::openHostApp(true);
				refreshStatus();
				return;
			}
#endif
			showError(T("Dock.VirtualCam"), error);
		}
	});

	connect(&v, &Vertical::canvasChanged, this, [this] {
		preview_->setCanvas(Vertical::instance().canvas());
		refreshHeader();
	});
	connect(&v, &Vertical::scenesChanged, this, &Dock::rebuildScenes);
	connect(&v, &Vertical::currentSceneChanged, this, &Dock::rebuildScenes);
	connect(&v, &Vertical::linksChanged, this, &Dock::rebuildScenes);
	connect(&v, &Vertical::settingsChanged, this, [this] {
		const bool on = Vertical::instance().settings().previewVisible;
		previewToggle_->setChecked(on);
		linked_->setChecked(Vertical::instance().linkedMode());
		rebuildDestinations();
		refreshHeader();
	});
	connect(&v, &Vertical::renderingChanged, this, [this](bool) { refreshHeader(); });
	connect(&o, &Outputs::statusChanged, this, &Dock::refreshStatus);
	connect(&o, &Outputs::outputError, this, &Dock::showError);
	connect(&o, &Outputs::replaySaved, this,
		[this](const QString &path) { encoderInfo_->setText(T("Replay.Saved").arg(path)); });

	preview_->setCanvas(v.canvas());
	rebuildScenes();
	rebuildDestinations();
	refreshHeader();
}

void Dock::refreshHeader()
{
	Vertical &v = Vertical::instance();
	const core::Size s = v.canvasSize();
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const double fps = ovi.fps_den ? (double)ovi.fps_num / ovi.fps_den : 0.0;
	QString text = QStringLiteral("<b>%1</b> · %2×%3 · %4 %5")
			       .arg(T("Dock.Title"))
			       .arg(s.width)
			       .arg(s.height)
			       .arg(QString::number(fps, 'g', 4))
			       .arg(T("Dock.Fps"));
	if (!v.canvas())
		text += QStringLiteral(" · ") + T("Dock.NoCanvas");
	else if (!v.rendering())
		text += QStringLiteral(" · ") + T("Dock.Idle");
	canvasBadge_->setText(text);
}

void Dock::rebuildScenes()
{
	rebuilding_ = true;
	Vertical &v = Vertical::instance();
	const QString current = v.currentScene();
	const auto &links = v.settings().links;
	scenes_->clear();
	for (const QString &name : v.sceneNames()) {
		QString label = name;
		const auto mains = links.mainScenesFor(name.toStdString());
		if (!mains.empty()) {
			QStringList m;
			for (const std::string &s : mains)
				m << QString::fromStdString(s);
			label += QStringLiteral("   ↔ ") + m.join(QStringLiteral(", "));
		}
		auto *item = new QListWidgetItem(label, scenes_);
		item->setData(Qt::UserRole, name);
		if (name == current) {
			QFont f = item->font();
			f.setBold(true);
			item->setFont(f);
			scenes_->setCurrentItem(item);
		}
	}
	rebuilding_ = false;
}

void Dock::rebuildDestinations()
{
	QLayoutItem *child;
	while ((child = destinationsLayout_->takeAt(0)) != nullptr) {
		delete child->widget();
		delete child;
	}
	const auto &dests = Vertical::instance().settings().destinations;
	for (const core::Destination &d : dests) {
		auto *row = new QWidget(destinationsBox_);
		row->setObjectName(QString::fromStdString(d.id));
		auto *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 0, 0, 0);
		auto *dot = new QLabel(row);
		dot->setObjectName(QStringLiteral("dot"));
		dot->setFixedSize(10, 10);
		auto *name = new QLabel(QStringLiteral("<b>%1</b> <span style='color:gray'>%2</span>")
						.arg(QString::fromStdString(d.name).toHtmlEscaped())
						.arg(QString::fromUtf8(core::platformInfo(d.platform).displayName)),
					row);
		auto *info = new QLabel(row);
		info->setObjectName(QStringLiteral("info"));
		auto *btn = new QPushButton(row);
		btn->setObjectName(QStringLiteral("toggle"));
		btn->setMinimumHeight(28);
		btn->setEnabled(d.enabled);
		const std::string id = d.id;
		connect(btn, &QPushButton::clicked, this, [id] { Outputs::instance().toggleDestination(id); });
		h->addWidget(dot);
		h->addWidget(name);
		h->addWidget(info, 1);
		h->addWidget(btn);
		destinationsLayout_->addWidget(row);
	}
	noDestinations_->setVisible(dests.empty());
	startAll_->setVisible(dests.size() > 1);
	stopAll_->setVisible(dests.size() > 1);
	refreshStatus();
}

void Dock::refreshStatus()
{
	Outputs &o = Outputs::instance();
	for (int i = 0; i < destinationsLayout_->count(); i++) {
		QWidget *row = destinationsLayout_->itemAt(i)->widget();
		if (!row)
			continue;
		const OutputStatus st = o.destinationStatus(row->objectName().toStdString());
		row->findChild<QLabel *>(QStringLiteral("dot"))->setStyleSheet(dotStyle(st.state));
		auto *info = row->findChild<QLabel *>(QStringLiteral("info"));
		info->setText(stateText(st));
		info->setToolTip(st.error);
		const bool running = st.state != OutputState::Idle && st.state != OutputState::Error;
		row->findChild<QPushButton *>(QStringLiteral("toggle"))
			->setText(running ? T("Dock.Stop") : T("Dock.Start"));
	}
	const OutputStatus rec = o.recordingStatus();
	record_->setChecked(o.recordingActive());
	record_->setText(o.recordingActive() ? T("Dock.StopRecord") : T("Dock.Record"));
	recordTime_->setText(rec.state == OutputState::Live
				     ? elapsed(rec.startedNs)
				     : (rec.state == OutputState::Error ? rec.error : QString()));
	replay_->setChecked(o.replayActive());
	saveReplay_->setEnabled(o.replayActive());
	virtualCam_->setChecked(o.virtualCamActive());
	virtualCam_->setText(o.virtualCamActive() ? T("Dock.StopVirtualCam") : T("Dock.VirtualCam"));
	encoderInfo_->setText(o.encoderSummary());
	refreshHeader();
}

void Dock::onSceneMenu(const QPoint &pos)
{
	QMenu menu(this);
	menu.addAction(T("Scene.Add"), this, &Dock::addScene);
	if (scenes_->itemAt(pos)) {
		menu.addAction(T("Scene.Rename"), this, &Dock::renameScene);
		menu.addAction(T("Scene.Remove"), this, &Dock::removeScene);
		menu.addSeparator();
		menu.addAction(T("Scene.LinkCurrent"), this, &Dock::linkCurrentMainScene);
		const QString name = scenes_->itemAt(pos)->data(Qt::UserRole).toString();
		for (const std::string &m : Vertical::instance().settings().links.mainScenesFor(name.toStdString())) {
			const QString main = QString::fromStdString(m);
			menu.addAction(T("Scene.Unlink").arg(main), this,
				       [main] { Vertical::instance().unlinkScene(main); });
		}
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
	const QString name = item->data(Qt::UserRole).toString();
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
	const QString from = item->data(Qt::UserRole).toString();
	bool ok = false;
	const QString to =
		QInputDialog::getText(this, T("Scene.Rename"), T("Scene.NamePrompt"), QLineEdit::Normal, from, &ok);
	if (!ok)
		return;
	QString error;
	if (!Vertical::instance().renameScene(from, to, &error) && !error.isEmpty())
		showError(T("Scene.Rename"), error);
}

void Dock::moveScene(int delta)
{
	const int row = scenes_->currentRow();
	if (row < 0)
		return;
	Vertical::instance().moveScene(row, row + delta);
	scenes_->setCurrentRow(std::clamp(row + delta, 0, scenes_->count() - 1));
}

void Dock::linkCurrentMainScene()
{
	QListWidgetItem *item = scenes_->currentItem();
	const QString main = Vertical::instance().mainSceneName();
	if (!item || main.isEmpty())
		return;
	Vertical::instance().linkScene(main, item->data(Qt::UserRole).toString());
}

void Dock::openSettings()
{
	SettingsDialog dlg(this);
	dlg.exec();
}

void Dock::showError(const QString &title, const QString &message)
{
	QMessageBox::warning(this, title.isEmpty() ? T("Dock.Title") : title, message);
}

} // namespace vertical::ui
