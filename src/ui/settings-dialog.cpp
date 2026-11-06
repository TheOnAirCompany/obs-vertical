#include "ui/settings-dialog.hpp"

#include "obs/hotkeys.hpp"
#include "obs/outputs.hpp"
#include "obs/text.hpp"
#include "obs/vertical.hpp"
#include "ui/destination-dialog.hpp"

#include <util/platform.h>

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace vertical::ui {

namespace {

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

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent), work_(Vertical::instance().settings())
{
	setWindowTitle(T("Settings.Title"));
	setMinimumWidth(520);
	auto *tabs = new QTabWidget(this);
	tabs->addTab(buildCanvasTab(), T("Settings.Canvas"));
	tabs->addTab(buildDestinationsTab(), T("Settings.Destinations"));
	tabs->addTab(buildRecordingTab(), T("Settings.Recording"));

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(tabs);
	layout->addWidget(buttons);
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
