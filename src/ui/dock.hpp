#pragma once

/* The "Vertical" dock (0.2): canvas header, preview, vertical scenes with their
 * links, destinations, recording / replay buffer / virtual camera and a status
 * bar. Two layouts: Studio (two columns) and Compact (one column), picked by the
 * dock width unless forced in the ⋯ menu. Design switches: ui/dock-decisions.hpp. */

#include "core/dock-model.hpp"

#include <util/platform.h>

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <string>
#include <vector>

class QAbstractButton;
class QBoxLayout;
class QFrame;
class QHBoxLayout;
class QLabel;
class QMenu;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

namespace vertical::ui {

class Chip;
class DestinationRow;
class ElidedLabel;
class PreviewFrame;
class PreviewWindow;
class SceneList;
class StatusDot;
class Switch;

class Dock : public QWidget {
	Q_OBJECT

public:
	explicit Dock(QWidget *parent = nullptr);
	~Dock() override;

	core::DockMode mode() const { return mode_; }
	core::LayoutChoice layoutChoice() const { return choice_; }
	void setLayoutChoice(core::LayoutChoice choice);

	/* Opens the link menu of a vertical scene (non-blocking popup). */
	QMenu *openLinkMenu(const QString &verticalScene, const QPoint &globalPos);
	/* The detached preview window, when open. */
	PreviewWindow *previewWindow() const;
	void openPreviewWindow();

protected:
	void resizeEvent(QResizeEvent *e) override;
	void changeEvent(QEvent *e) override;

private:
	/* construction */
	QWidget *buildHeader();
	QWidget *buildScenesHeader();
	QWidget *buildOutputsHeader();
	QWidget *buildDestinations();
	QWidget *buildTabs();
	QWidget *buildRecordPanel();
	QWidget *buildReplayPanel();
	QWidget *buildVirtualCamPanel();
	QWidget *buildLines();
	QWidget *buildFooter();
	QMenu *buildMoreMenu();

	/* layout */
	void applyMode(core::DockMode mode);
	void updateMode();
	void applyColors();

	/* content */
	void rebuildScenes();
	void rebuildDestinations();
	void refreshStatus();
	void refreshHeader();
	void refreshTimers();
	void refreshFooter();
	void refreshVirtualCam();

	/* actions */
	void onSceneMenu(const QPoint &pos);
	void addScene();
	void removeScene();
	void renameScene();
	void toggleRecording();
	void toggleReplay();
	void virtualCamAction();
	void addDestination(core::Platform p);
	void editDestination(const std::string &id);
	void duplicateDestination(const std::string &id);
	void deleteDestination(const std::string &id);
	void openSettings(int tab = 0);
	void showError(const QString &title, const QString &message);
	core::StatusTexts statusTexts() const;
	core::StatusView viewFor(const std::string &outputId, bool isDestination) const;
	core::VcamState vcamState() const;

	core::DockMode mode_ = core::DockMode::Studio;
	core::LayoutChoice choice_ = core::LayoutChoice::Automatic;
	bool rebuilding_ = false;

	/* frame */
	QWidget *header_ = nullptr;
	QWidget *body_ = nullptr;
	QBoxLayout *bodyLayout_ = nullptr;
	QWidget *colA_ = nullptr;
	QWidget *colB_ = nullptr;
	QVBoxLayout *colALayout_ = nullptr;
	QVBoxLayout *colBLayout_ = nullptr;
	QFrame *divider_ = nullptr;
	QWidget *footer_ = nullptr;

	/* header */
	QLabel *phoneIcon_ = nullptr;
	Chip *canvasChip_ = nullptr;
	QLabel *linkedLabel_ = nullptr;
	Switch *linked_ = nullptr;
	QToolButton *more_ = nullptr;
	QMenu *moreMenu_ = nullptr;

	/* preview and scenes */
	PreviewFrame *preview_ = nullptr;
	QPointer<PreviewWindow> previewWindow_;
	QWidget *scenesHeader_ = nullptr;
	QLabel *scenesCount_ = nullptr;
	QToolButton *addSceneButton_ = nullptr;
	SceneList *scenes_ = nullptr;

	/* destinations */
	QWidget *outputsHeader_ = nullptr;
	QLabel *outputsCount_ = nullptr;
	QPushButton *startAll_ = nullptr;
	QPushButton *stopAll_ = nullptr;
	QFrame *destinationsBox_ = nullptr;
	QScrollArea *destinationsScroll_ = nullptr;
	QWidget *destinationsList_ = nullptr;
	QVBoxLayout *destinationsLayout_ = nullptr;
	QLabel *noDestinations_ = nullptr;
	QToolButton *addDestination_ = nullptr;
	std::vector<DestinationRow *> rows_;

	/* tabs (Studio) */
	QWidget *tabsBox_ = nullptr;
	QAbstractButton *tabButtons_[3] = {nullptr, nullptr, nullptr};
	QStackedWidget *tabStack_ = nullptr;
	ElidedLabel *recordDetail_ = nullptr;
	QPushButton *recordButton_ = nullptr;
	QLabel *recordTimer_ = nullptr;
	ElidedLabel *recordError_ = nullptr;
	ElidedLabel *replayDetail_ = nullptr;
	QPushButton *replayButton_ = nullptr;
	QPushButton *saveReplay_ = nullptr;
	ElidedLabel *replayInfo_ = nullptr;
	StatusDot *vcamDot_ = nullptr;
	ElidedLabel *vcamStatus_ = nullptr;
	QPushButton *vcamButton_ = nullptr;
	QLabel *vcamNote_ = nullptr;

	/* lines (Compact) */
	QWidget *linesBox_ = nullptr;
	QLabel *recLineIcon_ = nullptr;
	ElidedLabel *recLine_ = nullptr;
	QToolButton *recLineButton_ = nullptr;
	ElidedLabel *replayLine_ = nullptr;
	QToolButton *replayLineSave_ = nullptr;
	QToolButton *replayLineButton_ = nullptr;
	QWidget *vcamLine_ = nullptr;
	StatusDot *vcamLineDot_ = nullptr;
	QToolButton *vcamLineButton_ = nullptr;

	/* footer */
	QLabel *outputsLive_ = nullptr;
	QLabel *cpu_ = nullptr;
	QLabel *encoderWarning_ = nullptr; /* only when the video is encoded on the CPU */
	QLabel *version_ = nullptr;
	QToolButton *gear_ = nullptr;
	QWidget *footerSecond_ = nullptr;

	QTimer clock_;
	os_cpu_usage_info_t *cpuInfo_ = nullptr;
	double cpuValue_ = -1.0;
	bool vcamInstalled_ = false;
	bool hardwareAvailable_ = false;
	bool vcamInstallRequested_ = false;
	int vcamPollTicks_ = 0;
};

} // namespace vertical::ui
