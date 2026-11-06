#pragma once

/* Settings of the vertical output: canvas, destinations, recording and replay, and the
 * vertical audio (sources sent to the vertical mix, Vertical Camera & Audio sync). */

#include "core/settings.hpp"
#include "obs/av-sync.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;

namespace vertical::ui {

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	enum class Tab { Canvas = 0, Destinations = 1, Recording = 2, Audio = 3 };
	explicit SettingsDialog(QWidget *parent = nullptr, Tab tab = Tab::Canvas);
	~SettingsDialog() override;

	void accept() override;
	void reject() override;

private:
	QWidget *buildCanvasTab();
	QWidget *buildDestinationsTab();
	QWidget *buildRecordingTab();
	QWidget *buildAudioTab();
	void refreshAudioSources();
	void refreshAudioStatus();
	void updateOffsetNote();
	void refreshDestinations();
	void addDestination(core::Platform p);
	void editDestination();
	void removeDestination();
	void duplicateDestination();
	void onPresetChanged();

	core::Settings work_;

	QComboBox *preset_;
	QSpinBox *width_;
	QSpinBox *height_;
	QComboBox *outputHeight_;
	QLabel *canvasNote_;

	QListWidget *destinations_;

	QLineEdit *recordPath_;
	QComboBox *recordFormat_;
	QSpinBox *recordKbps_;
	QSpinBox *replaySeconds_;
	QSpinBox *replayMb_;

	QLabel *audioTrack_;
	QScrollArea *audioScroll_;
	QWidget *audioSources_;
	QVBoxLayout *audioSourcesLayout_;
	QLabel *audioStatus_;
	QSpinBox *offset_;
	QPushButton *offsetDefault_;
	QLabel *offsetNote_;
	QPushButton *syncTest_;
	QLabel *syncNote_;
	bool offsetAutomatic_ = true;
	avsync::Config initialSync_;
	bool updatingOffset_ = false;
};

} // namespace vertical::ui
