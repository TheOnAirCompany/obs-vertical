#include "ui/settings-dialog.hpp"

#include "obs/audio-mix.hpp"
#include "obs/av-sync.hpp"
#include "obs/hotkeys.hpp"
#include "obs/outputs.hpp"
#include "obs/sync-test.hpp"
#include "obs/text.hpp"
#include "obs/vertical.hpp"
#include "obs/virtualaudio.hpp"
#include "ui/destination-dialog.hpp"

#include <util/platform.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace vertical::ui {

namespace {

/* provisional: dialog width and layout of the Audio tab (OBS's own settings use
 * plain forms). */
constexpr int kDialogMinWidth = 600;
constexpr int kDialogMinHeight = 760; /* the Audio tab and its wrapped notes fit */
constexpr int kAudioListMinHeight = 160;
constexpr int kAudioListPadding = 8;
constexpr int kOffsetFieldWidth = 110;

QString destinationLabel(const core::Destination &d)
{
	QString label = QStringLiteral("%1 — %2 · %3 kbps")
				.arg(QString::fromStdString(d.name))
				.arg(QString::fromUtf8(core::platformInfo(d.platform).displayName))
				.arg(d.encoding.video.bitrateKbps);
	if (!d.enabled)
		label += QStringLiteral(" · ") + T("Destination.Disabled");
	if (core::hasErrors(core::validate(d)))
		label += QStringLiteral(" · ⚠");
	return label;
}

std::vector<std::string> takenIds(const core::Settings &s)
{
	return s.destinationIds();
}

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent, Tab tab) : QDialog(parent), work_(Vertical::instance().settings())
{
	setWindowTitle(T("Settings.Title"));
	setMinimumSize(kDialogMinWidth, kDialogMinHeight);
	auto *tabs = new QTabWidget(this);
	tabs->addTab(buildCanvasTab(), T("Settings.Canvas"));
	tabs->addTab(buildDestinationsTab(), T("Settings.Destinations"));
	tabs->addTab(buildRecordingTab(), T("Settings.Recording"));
	tabs->addTab(buildAudioTab(), T("Settings.Audio"));
	tabs->setCurrentIndex((int)tab);
	/* vertical-sync-measure may trigger the sync test while this dialog is open. */
	synctest::setRemoteTrigger(true);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(tabs);
	layout->addWidget(buttons);
}

SettingsDialog::~SettingsDialog()
{
	synctest::setRemoteTrigger(false);
}

void SettingsDialog::reject()
{
	/* A sync test applied the offset being edited: put the saved one back. */
	const avsync::Config now = avsync::config();
	if (now.automatic != initialSync_.automatic || now.offsetMs != initialSync_.offsetMs)
		avsync::setConfig(initialSync_);
	QDialog::reject();
}

/* ---------------------------------------------------------------- canvas */

QWidget *SettingsDialog::buildCanvasTab()
{
	auto *w = new QWidget(this);
	auto *form = new QFormLayout(w);

	preset_ = new QComboBox(w);
	for (const core::CanvasPreset &p : core::canvasPresets())
		preset_->addItem(T(p.labelKey), QString::fromUtf8(p.id));
	preset_->addItem(T("Canvas.Preset.Custom"), QString());
	form->addRow(T("Canvas.Size"), preset_);

	auto *custom = new QHBoxLayout();
	width_ = new QSpinBox(w);
	height_ = new QSpinBox(w);
	for (QSpinBox *s : {width_, height_}) {
		s->setRange(core::kMinCanvasSide, core::kMaxCanvasSide);
		s->setSingleStep(2);
	}
	width_->setValue(work_.canvas.size.width);
	height_->setValue(work_.canvas.size.height);
	custom->addWidget(width_);
	custom->addWidget(new QLabel(QStringLiteral("×"), w));
	custom->addWidget(height_);
	custom->addStretch();
	form->addRow(QString(), custom);

	outputHeight_ = new QComboBox(w);
	outputHeight_->addItem(T("Canvas.Output.Same"), 0);
	for (int h : {2560, 1920, 1280, 960})
		outputHeight_->addItem(QString::number(h) + QStringLiteral(" p"), h);
	const int oh = outputHeight_->findData(work_.canvas.outputHeight);
	outputHeight_->setCurrentIndex(oh < 0 ? 0 : oh);
	form->addRow(T("Canvas.Output"), outputHeight_);

	canvasNote_ = new QLabel(w);
	canvasNote_->setWordWrap(true);
	form->addRow(QString(), canvasNote_);

	const core::CanvasPreset *p = core::findPreset(work_.canvas.size);
	preset_->setCurrentIndex(p ? preset_->findData(QString::fromUtf8(p->id)) : preset_->count() - 1);
	connect(preset_, &QComboBox::currentIndexChanged, this, &SettingsDialog::onPresetChanged);
	onPresetChanged();
	return w;
}

void SettingsDialog::onPresetChanged()
{
	const core::Size s = core::parseSize(preset_->currentData().toString().toStdString());
	const bool custom = s.width == 0;
	width_->setEnabled(custom);
	height_->setEnabled(custom);
	if (!custom) {
		width_->setValue(s.width);
		height_->setValue(s.height);
	}
	const bool busy = Outputs::instance().activeCount() > 0;
	canvasNote_->setText(busy ? T("Canvas.BusyResize") : T("Canvas.Note"));
}

/* ---------------------------------------------------------------- destinations */

QWidget *SettingsDialog::buildDestinationsTab()
{
	auto *w = new QWidget(this);
	auto *layout = new QVBoxLayout(w);
	destinations_ = new QListWidget(w);
	layout->addWidget(destinations_, 1);

	auto *row = new QHBoxLayout();
	auto *add = new QPushButton(T("Destination.Add"), w);
	auto *menu = new QMenu(add);
	for (core::Platform p : core::allPlatforms())
		menu->addAction(QString::fromUtf8(core::platformInfo(p).displayName), this,
				[this, p] { addDestination(p); });
	add->setMenu(menu);
	auto *edit = new QPushButton(T("Destination.Edit"), w);
	auto *dup = new QPushButton(T("Destination.Duplicate"), w);
	auto *remove = new QPushButton(T("Destination.Remove"), w);
	connect(edit, &QPushButton::clicked, this, &SettingsDialog::editDestination);
	connect(dup, &QPushButton::clicked, this, &SettingsDialog::duplicateDestination);
	connect(remove, &QPushButton::clicked, this, &SettingsDialog::removeDestination);
	connect(destinations_, &QListWidget::itemDoubleClicked, this, &SettingsDialog::editDestination);
	row->addWidget(add);
	row->addWidget(edit);
	row->addWidget(dup);
	row->addStretch();
	row->addWidget(remove);
	layout->addLayout(row);

	auto *note = new QLabel(T("Destination.SharingNote"), w);
	note->setWordWrap(true);
	layout->addWidget(note);
	refreshDestinations();
	return w;
}

void SettingsDialog::refreshDestinations()
{
	const int row = destinations_->currentRow();
	destinations_->clear();
	for (const core::Destination &d : work_.destinations) {
		auto *item = new QListWidgetItem(destinationLabel(d), destinations_);
		item->setData(Qt::UserRole, QString::fromStdString(d.id));
	}
	if (destinations_->count())
		destinations_->setCurrentRow(std::clamp(row, 0, destinations_->count() - 1));
}

void SettingsDialog::addDestination(core::Platform p)
{
	const std::string id = core::makeId((unsigned long long)QDateTime::currentMSecsSinceEpoch(), takenIds(work_));
	DestinationDialog dlg(core::makeDestination(p, id, core::platformInfo(p).displayName), this);
	if (dlg.exec() != QDialog::Accepted)
		return;
	work_.destinations.push_back(dlg.destination());
	refreshDestinations();
	destinations_->setCurrentRow(destinations_->count() - 1);
}

void SettingsDialog::editDestination()
{
	const int row = destinations_->currentRow();
	if (row < 0 || row >= (int)work_.destinations.size())
		return;
	DestinationDialog dlg(work_.destinations[(size_t)row], this);
	if (dlg.exec() != QDialog::Accepted)
		return;
	work_.destinations[(size_t)row] = dlg.destination();
	refreshDestinations();
}

void SettingsDialog::duplicateDestination()
{
	const int row = destinations_->currentRow();
	if (row < 0 || row >= (int)work_.destinations.size())
		return;
	core::Destination copy = work_.destinations[(size_t)row];
	copy.id = core::makeId((unsigned long long)QDateTime::currentMSecsSinceEpoch(), takenIds(work_));
	copy.name += " 2";
	copy.streamKey.clear(); /* two outputs never push to the same key */
	work_.destinations.push_back(copy);
	refreshDestinations();
}

void SettingsDialog::removeDestination()
{
	const int row = destinations_->currentRow();
	if (row < 0 || row >= (int)work_.destinations.size())
		return;
	const core::Destination &d = work_.destinations[(size_t)row];
	if (Outputs::instance().destinationStatus(d.id).state != OutputState::Idle &&
	    Outputs::instance().destinationStatus(d.id).state != OutputState::Error) {
		QMessageBox::warning(this, windowTitle(), T("Destination.BusyRemove"));
		return;
	}
	work_.destinations.erase(work_.destinations.begin() + row);
	refreshDestinations();
}

/* ---------------------------------------------------------------- recording */

QWidget *SettingsDialog::buildRecordingTab()
{
	auto *w = new QWidget(this);
	auto *form = new QFormLayout(w);

	auto *pathRow = new QHBoxLayout();
	recordPath_ = new QLineEdit(QString::fromStdString(work_.recording.path), w);
	recordPath_->setPlaceholderText(T("Recording.PathDefault"));
	auto *browse = new QPushButton(T("Recording.Browse"), w);
	connect(browse, &QPushButton::clicked, this, [this] {
		const QString dir = QFileDialog::getExistingDirectory(this, T("Recording.Path"), recordPath_->text());
		if (!dir.isEmpty())
			recordPath_->setText(dir);
	});
	pathRow->addWidget(recordPath_, 1);
	pathRow->addWidget(browse);
	form->addRow(T("Recording.Path"), pathRow);

	recordFormat_ = new QComboBox(w);
	for (core::RecordFormat f : {core::RecordFormat::HybridMp4, core::RecordFormat::HybridMov,
				     core::RecordFormat::Mkv, core::RecordFormat::Mp4, core::RecordFormat::Mov}) {
		const QString id = QString::fromUtf8(core::recordFormatId(f));
		recordFormat_->addItem(T(QStringLiteral("Recording.Format.") + id), id);
	}
	recordFormat_->setCurrentIndex(
		recordFormat_->findData(QString::fromUtf8(core::recordFormatId(work_.recording.format))));
	form->addRow(T("Recording.Format"), recordFormat_);

	recordKbps_ = new QSpinBox(w);
	recordKbps_->setRange(500, 200000);
	recordKbps_->setSingleStep(1000);
	recordKbps_->setSuffix(QStringLiteral(" kbps"));
	recordKbps_->setValue(work_.recording.encoding.video.bitrateKbps);
	form->addRow(T("Recording.Bitrate"), recordKbps_);

	replaySeconds_ = new QSpinBox(w);
	replaySeconds_->setRange(5, 21600);
	replaySeconds_->setSuffix(QStringLiteral(" s"));
	replaySeconds_->setValue(work_.replay.seconds);
	form->addRow(T("Replay.Length"), replaySeconds_);

	replayMb_ = new QSpinBox(w);
	replayMb_->setRange(16, 65536);
	replayMb_->setSuffix(QStringLiteral(" MB"));
	replayMb_->setValue(work_.replay.maxMb);
	form->addRow(T("Replay.MaxSize"), replayMb_);

	auto *note = new QLabel(T("Recording.SharingNote"), w);
	note->setWordWrap(true);
	form->addRow(QString(), note);
	return w;
}

/* ---------------------------------------------------------------- audio */

QWidget *SettingsDialog::buildAudioTab()
{
	auto *w = new QWidget(this);
	auto *layout = new QVBoxLayout(w);

	/* ---- the vertical mix ---- */
	auto *mix = new QGroupBox(T("Audio.MixTitle"), w);
	auto *mixLayout = new QVBoxLayout(mix);
	audioTrack_ = new QLabel(mix);
	audioTrack_->setObjectName(QStringLiteral("verticalAudioTrack"));
	audioTrack_->setWordWrap(true);
	mixLayout->addWidget(audioTrack_);
	auto *listTitle = new QLabel(T("Audio.SourcesTitle"), mix);
	mixLayout->addWidget(listTitle);
	/* One OBS-style checkbox per source, in a scroll area. */
	audioScroll_ = new QScrollArea(mix);
	audioScroll_->setWidgetResizable(true);
	audioScroll_->setMinimumHeight(kAudioListMinHeight);
	audioSources_ = new QWidget(audioScroll_);
	audioSources_->setObjectName(QStringLiteral("verticalAudioSources"));
	audioSourcesLayout_ = new QVBoxLayout(audioSources_);
	audioSourcesLayout_->setContentsMargins(kAudioListPadding, kAudioListPadding, kAudioListPadding,
						kAudioListPadding);
	audioScroll_->setWidget(audioSources_);
	mixLayout->addWidget(audioScroll_, 1);
	auto *mixNote = new QLabel(T("Audio.MixNote"), mix);
	mixNote->setWordWrap(true);
	mixLayout->addWidget(mixNote);
	layout->addWidget(mix, 1);

	/* ---- Vertical Camera & Audio ---- */
	/* "&" would be read as a mnemonic in a group box title. */
	auto *pair = new QGroupBox(T("Audio.PairTitle").replace(QStringLiteral("&"), QStringLiteral("&&")), w);
	auto *form = new QVBoxLayout(pair);
	audioStatus_ = new QLabel(pair);
	audioStatus_->setObjectName(QStringLiteral("verticalAudioStatus"));
	audioStatus_->setWordWrap(true);
	form->addWidget(audioStatus_);

	auto *offsetRow = new QHBoxLayout();
	offset_ = new QSpinBox(pair);
	offset_->setObjectName(QStringLiteral("verticalSyncOffset"));
	offset_->setRange(core::kMinOffsetMs, core::kMaxOffsetMs);
	offset_->setSuffix(QStringLiteral(" ms"));
	offset_->setMinimumWidth(kOffsetFieldWidth);
	offsetDefault_ = new QPushButton(T("Audio.OffsetDefault"), pair);
	offsetDefault_->setObjectName(QStringLiteral("verticalSyncOffsetDefault"));
	offsetRow->addWidget(new QLabel(T("Audio.Offset"), pair));
	offsetRow->addWidget(offset_);
	offsetRow->addWidget(offsetDefault_);
	offsetRow->addStretch(1);
	form->addLayout(offsetRow);
	offsetNote_ = new QLabel(pair);
	offsetNote_->setWordWrap(true);
	form->addWidget(offsetNote_);

	auto *testRow = new QHBoxLayout();
	syncTest_ = new QPushButton(T("Audio.SyncTest"), pair);
	syncTest_->setObjectName(QStringLiteral("verticalSyncTest"));
	syncNote_ = new QLabel(T("Audio.SyncTestNote"), pair);
	syncNote_->setWordWrap(true);
	testRow->addWidget(syncTest_);
	testRow->addWidget(syncNote_, 1);
	form->addLayout(testRow);
	layout->addWidget(pair);

	const avsync::Config cfg = avsync::config();
	initialSync_ = cfg;
	offsetAutomatic_ = cfg.automatic;
	offset_->setValue(cfg.automatic ? avsync::defaultOffsetMs() : cfg.offsetMs);
	connect(offset_, &QSpinBox::valueChanged, this, [this] {
		if (updatingOffset_)
			return;
		offsetAutomatic_ = false;
		updateOffsetNote();
	});
	connect(offsetDefault_, &QPushButton::clicked, this, [this] {
		offsetAutomatic_ = true;
		updatingOffset_ = true;
		offset_->setValue(avsync::defaultOffsetMs());
		updatingOffset_ = false;
		updateOffsetNote();
	});
	connect(syncTest_, &QPushButton::clicked, this, [this] {
		/* The offset being edited applies at once, so the test shows it. */
		avsync::setConfig({offsetAutomatic_, offset_->value()});
		synctest::trigger();
	});
	connect(&AudioMix::instance(), &AudioMix::sourcesChanged, this, &SettingsDialog::refreshAudioSources);
	connect(&AudioMix::instance(), &AudioMix::trackChanged, this, &SettingsDialog::refreshAudioSources);
	auto *poll = new QTimer(this);
	poll->setInterval(1000);
	connect(poll, &QTimer::timeout, this, &SettingsDialog::refreshAudioStatus);
	poll->start();

	refreshAudioSources();
	refreshAudioStatus();
	updateOffsetNote();
	return w;
}

void SettingsDialog::refreshAudioSources()
{
	AudioMix &mix = AudioMix::instance();
	audioTrack_->setText(T("Audio.Track").arg(mix.track()));
	const auto rows = core::audioRows(mix.mainSources(), work_.audio.exclusions);
	while (QLayoutItem *it = audioSourcesLayout_->takeAt(0)) {
		delete it->widget();
		delete it;
	}
	for (const core::AudioRow &r : rows) {
		auto *box = new QCheckBox(QString::fromStdString(r.name), audioSources_);
		box->setChecked(r.sent && r.onMainMix);
		box->setEnabled(r.onMainMix);
		box->setToolTip(r.onMainMix ? T("Audio.SentTip") : T("Audio.NotOnMainMix"));
		const std::string uuid = r.uuid;
		connect(box, &QCheckBox::toggled, this,
			[this, uuid](bool on) { work_.audio.exclusions.setExcluded(uuid, !on); });
		audioSourcesLayout_->addWidget(box);
	}
	if (rows.empty())
		audioSourcesLayout_->addWidget(new QLabel(T("Audio.NoSources"), audioSources_));
	audioSourcesLayout_->addStretch(1);
}

void SettingsDialog::refreshAudioStatus()
{
	const virtualaudio::Stats s = virtualaudio::stats();
	QString text;
	if (!s.installed)
		text = T("Audio.Status.NotInstalled");
	else if (s.feeding)
		text = T("Audio.Status.Feeding").arg(qRound(s.scheduleMs + s.audioDelayMs));
	else
		text = T("Audio.Status.Idle");
	audioStatus_->setText(
		QStringLiteral("<b>%1</b> · %2").arg(T("Audio.Microphone").toHtmlEscaped(), text.toHtmlEscaped()));
	if (offsetAutomatic_) {
		updatingOffset_ = true;
		offset_->setValue(avsync::defaultOffsetMs());
		updatingOffset_ = false;
	}
	updateOffsetNote();
}

void SettingsDialog::updateOffsetNote()
{
	const bool measured = avsync::audioMeasured() || avsync::videoMeasured();
	const int d = avsync::defaultOffsetMs();
	const QString value = (d > 0 ? QStringLiteral("+") : QString()) + QString::number(d);
	offsetNote_->setText(
		T("Audio.OffsetNote").arg(value).arg(measured ? T("Audio.OffsetMeasured") : T("Audio.OffsetEstimated")) +
		(offsetAutomatic_ ? QStringLiteral(" ") + T("Audio.OffsetAutomatic") : QString()));
	offsetDefault_->setEnabled(!offsetAutomatic_);
}

/* ---------------------------------------------------------------- apply */

void SettingsDialog::accept()
{
	const core::Size size{width_->value(), height_->value()};
	if (core::isSizeError(core::checkCanvasSize(size))) {
		QMessageBox::warning(this, windowTitle(), T("Canvas.InvalidSize"));
		return;
	}
	work_.recording.path = recordPath_->text().trimmed().toStdString();
	work_.recording.format = core::recordFormatFromId(recordFormat_->currentData().toString().toStdString());
	work_.recording.encoding.video.bitrateKbps = recordKbps_->value();
	work_.replay.seconds = replaySeconds_->value();
	work_.replay.maxMb = replayMb_->value();

	Vertical &v = Vertical::instance();
	const int outputHeight = outputHeight_->currentData().toInt();
	const bool sizeChanged = size != v.settings().canvas.size || outputHeight != v.settings().canvas.outputHeight;

	/* Copy everything except what the canvas owns (applied below). */
	core::Settings &s = v.settings();
	if (work_.audio.exclusions != s.audio.exclusions)
		AudioMix::instance().setExclusions(work_.audio.exclusions);
	avsync::setConfig({offsetAutomatic_, offset_->value()});
	s.destinations = work_.destinations;
	s.recording = work_.recording;
	s.replay = work_.replay;
	if (sizeChanged) {
		QString error;
		if (!v.applyCanvasSize(size, outputHeight, &error)) {
			QMessageBox::warning(this, windowTitle(), error);
			return;
		}
	}
	v.settingsEdited();
	hotkeys::syncDestinations();
	QDialog::accept();
}

} // namespace vertical::ui
