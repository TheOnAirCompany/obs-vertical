#include "ui/destination-dialog.hpp"

#include "core/dock-model.hpp"
#include "obs/audio-mix.hpp"
#include "obs/outputs.hpp"
#include "obs/text.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace vertical::ui {

DestinationDialog::DestinationDialog(const core::Destination &d, QWidget *parent) : QDialog(parent), base_(d)
{
	setWindowTitle(T("Destination.Title"));
	auto *form = new QFormLayout();

	name_ = new QLineEdit(QString::fromStdString(d.name), this);
	form->addRow(T("Destination.Name"), name_);

	platform_ = new QComboBox(this);
	for (core::Platform p : core::allPlatforms())
		platform_->addItem(QString::fromUtf8(core::platformInfo(p).displayName), core::platformId(p));
	platform_->setCurrentIndex(platform_->findData(core::platformId(d.platform)));
	form->addRow(T("Destination.Platform"), platform_);

	server_ = new QLineEdit(QString::fromStdString(d.server), this);
	server_->setPlaceholderText(QStringLiteral("rtmps://host:443/app"));
	form->addRow(T("Destination.Server"), server_);

	auto *keyRow = new QHBoxLayout();
	key_ = new QLineEdit(QString::fromStdString(d.streamKey), this);
	key_->setEchoMode(QLineEdit::Password);
	auto *show = new QToolButton(this);
	show->setText(T("Destination.ShowKey"));
	show->setCheckable(true);
	connect(show, &QToolButton::toggled, this,
		[this](bool on) { key_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
	keyRow->addWidget(key_, 1);
	keyRow->addWidget(show);
	form->addRow(T("Destination.Key"), keyRow);

	enabled_ = new QCheckBox(T("Destination.Enabled"), this);
	enabled_->setChecked(d.enabled);
	form->addRow(QString(), enabled_);

	videoKbps_ = new QSpinBox(this);
	videoKbps_->setRange(200, 100000);
	videoKbps_->setSingleStep(500);
	videoKbps_->setSuffix(QStringLiteral(" kbps"));
	videoKbps_->setValue(d.encoding.video.bitrateKbps);
	form->addRow(T("Destination.VideoBitrate"), videoKbps_);

	keyint_ = new QSpinBox(this);
	keyint_->setRange(0, 20);
	keyint_->setSuffix(QStringLiteral(" s"));
	keyint_->setValue(d.encoding.video.keyintSec);
	form->addRow(T("Destination.Keyframes"), keyint_);

	codec_ = new QComboBox(this);
	codec_->addItem(QStringLiteral("H.264"), QStringLiteral("h264"));
	codec_->addItem(QStringLiteral("HEVC"), QStringLiteral("hevc"));
	codec_->setCurrentIndex(codec_->findData(QString::fromStdString(d.encoding.video.codec)));
	form->addRow(T("Destination.Codec"), codec_);

	encoder_ = new QComboBox(this);
	encoder_->addItem(T("Destination.EncoderAuto"), QStringLiteral("auto"));
	for (const core::EncoderCandidate &c : Outputs::instance().videoEncoders()) {
		if (c.codec != "h264" && c.codec != "hevc")
			continue;
		QString label = QString::fromStdString(c.displayName.empty() ? c.id : c.displayName);
		if (!core::isHardwareEncoder(c))
			label += QStringLiteral(" (CPU)");
		encoder_->addItem(label, QString::fromStdString(c.id));
	}
	const int encIndex = encoder_->findData(QString::fromStdString(d.encoding.video.encoder));
	encoder_->setCurrentIndex(encIndex < 0 ? 0 : encIndex);
	form->addRow(T("Destination.Encoder"), encoder_);

	audioKbps_ = new QSpinBox(this);
	audioKbps_->setRange(32, 512);
	audioKbps_->setSingleStep(32);
	audioKbps_->setSuffix(QStringLiteral(" kbps"));
	audioKbps_->setValue(d.encoding.audio.bitrateKbps);
	form->addRow(T("Destination.AudioBitrate"), audioKbps_);

	/* The vertical mix (with the exclusions of the Audio settings) unless an expert
	 * picks a raw OBS track. */
	track_ = new QComboBox(this);
	track_->addItem(T("Destination.AudioTrackVertical").arg(AudioMix::instance().track()), 0);
	for (int t = 1; t <= core::kTrackCount; t++)
		track_->addItem(T("Destination.AudioTrackN").arg(t), t);
	const int ti = track_->findData(d.encoding.audio.track);
	track_->setCurrentIndex(ti < 0 ? 0 : ti);
	form->addRow(T("Destination.AudioTrack"), track_);

	issues_ = new QLabel(this);
	issues_->setWordWrap(true);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	ok_ = buttons->button(QDialogButtonBox::Ok);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(issues_);
	layout->addWidget(buttons);

	connect(platform_, &QComboBox::currentIndexChanged, this, &DestinationDialog::onPlatformChanged);
	connect(server_, &QLineEdit::textChanged, this, &DestinationDialog::onServerChanged);
	for (QLineEdit *e : {name_, server_, key_})
		connect(e, &QLineEdit::textChanged, this, &DestinationDialog::validateNow);
	connect(track_, &QComboBox::currentIndexChanged, this, &DestinationDialog::validateNow);
	for (QSpinBox *s : {videoKbps_, keyint_, audioKbps_})
		connect(s, &QSpinBox::valueChanged, this, &DestinationDialog::validateNow);
	connect(codec_, &QComboBox::currentIndexChanged, this, &DestinationDialog::validateNow);
	onServerChanged();
	validateNow();
}

/* A server that names its platform (a TikTok, YouTube, Instagram or Facebook
 * server, or srt:// for a custom server) selects that platform, so the platform
 * saved, and the tile of the dock, match the server. */
void DestinationDialog::onServerChanged()
{
	core::Destination probe;
	probe.platform = core::platformFromId(platform_->currentData().toString().toStdString());
	probe.server = server_->text().toStdString();
	const core::Platform want = core::tilePlatform(probe);
	if (want != probe.platform)
		platform_->setCurrentIndex(platform_->findData(QString::fromUtf8(core::platformId(want))));
}

void DestinationDialog::onPlatformChanged()
{
	const core::Platform p = core::platformFromId(platform_->currentData().toString().toStdString());
	const core::PlatformInfo &info = core::platformInfo(p);
	/* Only replace a server that is empty or the default of another platform. */
	bool isDefault = server_->text().trimmed().isEmpty();
	for (core::Platform other : core::allPlatforms())
		if (server_->text() == QString::fromUtf8(core::platformInfo(other).defaultServer))
			isDefault = true;
	if (isDefault)
		server_->setText(QString::fromUtf8(info.defaultServer));
	videoKbps_->setValue(std::min(videoKbps_->value(), info.maxKbps));
	validateNow();
}

core::Destination DestinationDialog::destination() const
{
	core::Destination d = base_;
	d.name = name_->text().trimmed().toStdString();
	d.platform = core::platformFromId(platform_->currentData().toString().toStdString());
	d.server = server_->text().trimmed().toStdString();
	d.streamKey = key_->text().trimmed().toStdString();
	d.enabled = enabled_->isChecked();
	d.encoding.video.bitrateKbps = videoKbps_->value();
	d.encoding.video.keyintSec = keyint_->value();
	d.encoding.video.codec = codec_->currentData().toString().toStdString();
	d.encoding.video.encoder = encoder_->currentData().toString().toStdString();
	d.encoding.audio.bitrateKbps = audioKbps_->value();
	d.encoding.audio.track = track_->currentData().toInt();
	return d;
}

void DestinationDialog::validateNow()
{
	const auto issues = core::validate(destination());
	QStringList lines;
	for (const core::Issue &i : issues)
		lines << (i.severity == core::Severity::Error ? QStringLiteral("✕ ") : QStringLiteral("⚠ ")) +
				 T(core::issueKey(i.code));
	issues_->setText(lines.join(QStringLiteral("\n")));
	/* Saving an incomplete destination is allowed (the key often comes later);
	 * starting it is refused while errors remain. Only a name is required. */
	if (ok_)
		ok_->setEnabled(!name_->text().trimmed().isEmpty());
}

} // namespace vertical::ui
