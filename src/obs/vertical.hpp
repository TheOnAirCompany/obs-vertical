#pragma once

/* The vertical canvas and its scenes: lifecycle, persistence with the scene
 * collection, linked scenes and the render gate. UI thread only, except the
 * api*() functions which are safe from any thread. */

#include "core/render-gate.hpp"
#include "core/settings.hpp"

#include <obs.hpp>

#include <QObject>
#include <QString>
#include <QStringList>

#include <mutex>
#include <string>

namespace vertical {

class Vertical : public QObject {
	Q_OBJECT

public:
	static Vertical &instance();

	/* ---- lifecycle (called from plugin-main) ---- */
	void start();
	void shutdown();
	void load(obs_data_t *data);
	void save(obs_data_t *data);
	void onCollectionChanging();
	void onCollectionCleanup();
	void onCollectionReady();
	void onMainSceneChanged();

	core::Settings &settings() { return settings_; }
	const core::Settings &settings() const { return settings_; }
	/* Call after editing settings() from the UI. */
	void settingsEdited();

	/* ---- canvas ---- */
	obs_canvas_t *canvas() const { return canvas_; }
	bool ensureCanvas();
	core::Size canvasSize() const;
	core::Size outputSize() const;
	/* Resizes the canvas; refused while an output is running. */
	bool applyCanvasSize(const core::Size &size, int outputHeight, QString *error);
	/* Re-reads fps and colour settings of the main video (after Settings > Video). */
	bool resetVideo();

	/* ---- scenes (dock order) ---- */
	QStringList sceneNames() const;
	QString currentScene() const;
	void setCurrentScene(const QString &name);
	QString addScene(const QString &baseName);
	bool removeScene(const QString &name);
	bool renameScene(const QString &from, const QString &to, QString *error);
	void moveScene(int from, int to);
	OBSSourceAutoRelease sceneSource(const QString &name) const;

	/* ---- linked scenes ---- */
	bool linkedMode() const { return settings_.linkedScenes; }
	void setLinkedMode(bool on);
	bool linkScene(const QString &mainScene, const QString &verticalScene);
	void unlinkScene(const QString &mainScene);
	QString mainSceneName() const;

	/* ---- render gate ---- */
	void acquireRender(core::RenderReason r);
	void releaseRender(core::RenderReason r);
	bool rendering() const { return gate_.active(); }

	/* ---- public interface for other plugins (any thread) ---- */
	struct ApiCanvas {
		bool ready = false;
		std::string uuid;
		std::string name;
		int width = 0;
		int height = 0;
	};
	ApiCanvas apiCanvas() const;
	bool apiLinkScene(const std::string &mainScene, const std::string &verticalScene, std::string *error);

signals:
	void canvasChanged();
	void scenesChanged();
	void currentSceneChanged();
	void linksChanged();
	void renderingChanged(bool rendering);
	void settingsChanged();

private:
	Vertical() = default;
	void attachProgram();
	void detachProgram();
	void releaseCanvas();
	void refreshApiSnapshot();
	void connectGlobalSignals();
	void disconnectGlobalSignals();
	QStringList rawSceneNames() const;
	void onSourceRenamed(obs_source_t *source, const QString &from, const QString &to);
	void onSourceRemoved(obs_source_t *source);
	void registerSceneHotkey(obs_source_t *scene);

	static void sourceRenameCb(void *data, calldata_t *cd);
	static void sourceRemoveCb(void *data, calldata_t *cd);
	static void canvasRemoveCb(void *data, calldata_t *cd);
	static void canvasSourceAddCb(void *data, calldata_t *cd);

	core::Settings settings_;
	core::RenderGate gate_;
	obs_canvas_t *canvas_ = nullptr; /* strong reference */
	bool signalsConnected_ = false;
	bool loaded_ = false;
	bool switching_ = false; /* between SCENE_COLLECTION_CHANGING and the new collection */

	mutable std::mutex apiMutex_; /* guards api_ and settings_.links */
	ApiCanvas api_;
};

} // namespace vertical
