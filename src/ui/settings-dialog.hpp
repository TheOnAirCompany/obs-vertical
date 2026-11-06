#pragma once

/* Settings of the vertical output: canvas, destinations, recording and replay. */

#include "core/settings.hpp"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;

namespace vertical::ui {

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	enum class Tab { Canvas = 0, Destinations = 1, Recording = 2 };
	explicit SettingsDialog(QWidget *parent = nullptr, Tab tab = Tab::Canvas);

	void accept() override;

private:
	QWidget *buildCanvasTab();
	QWidget *buildDestinationsTab();
	QWidget *buildRecordingTab();
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
};

} // namespace vertical::ui
