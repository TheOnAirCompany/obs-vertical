#pragma once

/* The "Vertical" dock: preview, vertical scenes, outputs and their status. */

#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace vertical::ui {

class Preview;

class Dock : public QWidget {
	Q_OBJECT

public:
	explicit Dock(QWidget *parent = nullptr);

private:
	void rebuildScenes();
	void rebuildDestinations();
	void refreshStatus();
	void refreshHeader();
	void onSceneMenu(const QPoint &pos);
	void addScene();
	void removeScene();
	void renameScene();
	void moveScene(int delta);
	void linkCurrentMainScene();
	void openSettings();
	void showError(const QString &title, const QString &message);

	QLabel *canvasBadge_;
	QToolButton *previewToggle_;
	Preview *preview_;
	QListWidget *scenes_;
	QCheckBox *linked_;
	QWidget *destinationsBox_;
	QVBoxLayout *destinationsLayout_;
	QLabel *noDestinations_;
	QPushButton *startAll_;
	QPushButton *stopAll_;
	QPushButton *record_;
	QLabel *recordTime_;
	QPushButton *replay_;
	QPushButton *saveReplay_;
	QPushButton *virtualCam_;
	QLabel *encoderInfo_;
	bool rebuilding_ = false;
};

} // namespace vertical::ui
