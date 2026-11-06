#include "obs/vertical.hpp"

#include "obs/text.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMetaObject>
#include <QSet>

#include <algorithm>

namespace vertical {

namespace {

constexpr uint32_t kCanvasFlags = ACTIVATE | SCENE_REF; /* audio: see docs/ARCHITECTURE.md */
const char *kSaveKey = "vertical";

QSet<QString> g_hotkeyScenes; /* uuids of scenes with a "select" hotkey registered */

std::string toStd(const QString &s)
{
	return s.toUtf8().toStdString();
}

QString toQ(const char *s)
{
	return QString::fromUtf8(s ? s : "");
}

QString toQ(const std::string &s)
{
	return QString::fromStdString(s);
}

obs_video_info makeVideoInfo(const core::Size &base, const core::Size &out)
{
	obs_video_info ovi = {};
	obs_get_video_info(&ovi); /* fps, formats, colour space and scaling of the main video */
	ovi.base_width = (uint32_t)base.width;
	ovi.base_height = (uint32_t)base.height;
	ovi.output_width = (uint32_t)out.width;
	ovi.output_height = (uint32_t)out.height;
	return ovi;
}

void selectSceneHotkey(void *, obs_hotkey_id, obs_hotkey_t *hotkey, bool pressed)
{
	if (!pressed)
		return;
	auto *weak = static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(hotkey));
	OBSSourceAutoRelease source = obs_weak_source_get_source(weak);
	if (!source)
		return;
	const QString name = toQ(obs_source_get_name(source));
	QMetaObject::invokeMethod(
		&Vertical::instance(), [name] { Vertical::instance().setCurrentScene(name); }, Qt::QueuedConnection);
}

} // namespace

Vertical &Vertical::instance()
{
	static Vertical *v = new Vertical(); /* lives until the process ends */
	return *v;
}

/* =================================================================== lifecycle */

void Vertical::start()
{
	connectGlobalSignals();
}

void Vertical::shutdown()
{
	disconnectGlobalSignals();
	detachProgram();
	releaseCanvas();
}

void Vertical::load(obs_data_t *data)
{
	OBSDataAutoRelease obj = obs_data_get_obj(data, kSaveKey);
	std::vector<std::string> warnings;
	core::Settings loaded = obj ? core::loadSettings(obs_data_get_json(obj), &warnings) : core::Settings();
	for (const std::string &w : warnings)
		obs_log(LOG_WARNING, "settings: %s", w.c_str());
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		settings_ = std::move(loaded);
	}
	loaded_ = true;
	emit settingsChanged();
}

void Vertical::save(obs_data_t *data)
{
	if (canvas_) {
		settings_.canvas.uuid = obs_canvas_get_uuid(canvas_);
		settings_.canvas.name = obs_canvas_get_name(canvas_);
	}
	settings_.sceneOrder.clear();
	for (const QString &n : sceneNames())
		settings_.sceneOrder.push_back(toStd(n));
	std::string text;
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		text = core::saveSettings(settings_);
	}
	OBSDataAutoRelease obj = obs_data_create_from_json(text.c_str());
	if (obj)
		obs_data_set_obj(data, kSaveKey, obj);
}

void Vertical::onCollectionChanging()
{
	/* Outputs were stopped by the caller; let go of the scene on air before
	 * the frontend removes every source. */
	switching_ = true;
	detachProgram();
}

void Vertical::onCollectionCleanup()
{
	detachProgram();
	releaseCanvas();
	g_hotkeyScenes.clear();
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		settings_ = core::Settings();
	}
	loaded_ = false;
	refreshApiSnapshot();
	emit canvasChanged();
	emit scenesChanged();
	emit settingsChanged();
}

void Vertical::onCollectionReady()
{
	switching_ = false;
	loaded_ = true; /* also for a new, empty collection (no load callback) */
	ensureCanvas();
	emit settingsChanged();
}

void Vertical::onMainSceneChanged()
{
	const QStringList scenes = sceneNames();
	std::vector<std::string> names;
	for (const QString &s : scenes)
		names.push_back(toStd(s));
	core::SwitchDecision d;
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		d = core::decideSwitch(settings_.links, settings_.linkedScenes, toStd(mainSceneName()),
				       settings_.currentScene, names);
	}
	if (d.doSwitch)
		setCurrentScene(toQ(d.target));
}

void Vertical::settingsEdited()
{
	refreshApiSnapshot();
	emit settingsChanged();
}

/* =================================================================== canvas */

bool Vertical::ensureCanvas()
{
	if (canvas_ && !obs_canvas_removed(canvas_))
		return true;
	releaseCanvas();

	obs_canvas_t *found = nullptr;
	if (!settings_.canvas.uuid.empty()) {
		found = obs_get_canvas_by_uuid(settings_.canvas.uuid.c_str());
		if (found && obs_canvas_removed(found)) {
			obs_canvas_release(found);
			found = nullptr;
		}
	}

	if (found) {
		canvas_ = found;
		/* Canvases come back from the collection without video: give it ours. */
		resetVideo();
	} else {
		std::vector<std::string> taken;
		obs_enum_canvases(
			[](void *param, obs_canvas_t *c) {
				static_cast<std::vector<std::string> *>(param)->push_back(obs_canvas_get_name(c));
				return true;
			},
			&taken);
		const std::string name = core::uniqueName(settings_.canvas.name, taken);
		obs_video_info ovi = makeVideoInfo(canvasSize(), outputSize());
		canvas_ = obs_frontend_add_canvas(name.c_str(), &ovi, kCanvasFlags);
		if (!canvas_) {
			obs_log(LOG_ERROR, "could not create the vertical canvas");
			return false;
		}
		settings_.canvas.uuid = obs_canvas_get_uuid(canvas_);
		settings_.canvas.name = obs_canvas_get_name(canvas_);
		obs_log(LOG_INFO, "created canvas '%s' %dx%d", name.c_str(), canvasSize().width, canvasSize().height);
	}

	/* Renames inside a canvas other than the main one are only signalled on the canvas. */
	signal_handler_connect(obs_canvas_get_signal_handler(canvas_), "remove", canvasRemoveCb, this);
	signal_handler_connect(obs_canvas_get_signal_handler(canvas_), "source_rename", sourceRenameCb, this);
	/* Scenes another plugin creates in this canvas (Layouts' vertical twin) appear at once. */
	signal_handler_connect(obs_canvas_get_signal_handler(canvas_), "source_add", canvasSourceAddCb, this);

	if (rawSceneNames().isEmpty())
		addScene(T("Scene.DefaultName"));
	for (const QString &n : rawSceneNames()) {
		OBSSourceAutoRelease s = sceneSource(n);
		registerSceneHotkey(s);
	}
	if (currentScene().isEmpty() || !rawSceneNames().contains(currentScene()))
		settings_.currentScene = toStd(sceneNames().value(0));

	if (gate_.active())
		attachProgram();
	refreshApiSnapshot();
	emit canvasChanged();
	emit scenesChanged();
	emit currentSceneChanged();
	return true;
}

void Vertical::releaseCanvas()
{
	if (!canvas_)
		return;
	signal_handler_disconnect(obs_canvas_get_signal_handler(canvas_), "remove", canvasRemoveCb, this);
	signal_handler_disconnect(obs_canvas_get_signal_handler(canvas_), "source_rename", sourceRenameCb, this);
	signal_handler_disconnect(obs_canvas_get_signal_handler(canvas_), "source_add", canvasSourceAddCb, this);
	obs_canvas_release(canvas_);
	canvas_ = nullptr;
	refreshApiSnapshot();
}

core::Size Vertical::canvasSize() const
{
	return settings_.canvas.size;
}

core::Size Vertical::outputSize() const
{
	return core::scaledOutput(settings_.canvas.size, settings_.canvas.outputHeight);
}

bool Vertical::resetVideo()
{
	if (!canvas_)
		return false;
	if (obs_canvas_video_active(canvas_))
		return false;
	obs_video_info ovi = makeVideoInfo(canvasSize(), outputSize());
	const bool ok = obs_canvas_reset_video(canvas_, &ovi);
	if (!ok)
		obs_log(LOG_ERROR, "could not reset the vertical canvas video (%ux%u)", ovi.base_width,
			ovi.base_height);
	return ok;
}

bool Vertical::applyCanvasSize(const core::Size &size, int outputHeight, QString *error)
{
	if (core::isSizeError(core::checkCanvasSize(size))) {
		if (error)
			*error = T("Canvas.InvalidSize");
		return false;
	}
	if (canvas_ && obs_canvas_video_active(canvas_)) {
		if (error)
			*error = T("Canvas.BusyResize");
		return false;
	}
	settings_.canvas.size = size;
	settings_.canvas.outputHeight = outputHeight;
	if (canvas_ && !resetVideo()) {
		if (error)
			*error = T("Canvas.ResetFailed");
		return false;
	}
	refreshApiSnapshot();
	emit canvasChanged();
	return true;
}

/* =================================================================== scenes */

QStringList Vertical::rawSceneNames() const
{
	QStringList names;
	if (!canvas_)
		return names;
	obs_canvas_enum_scenes(
		canvas_,
		[](void *param, obs_source_t *source) {
			static_cast<QStringList *>(param)->append(QString::fromUtf8(obs_source_get_name(source)));
			return true;
		},
		&names);
	return names;
}

QStringList Vertical::sceneNames() const
{
	std::vector<std::string> existing;
	for (const QString &n : rawSceneNames())
		existing.push_back(toStd(n));
	QStringList out;
	for (const std::string &n : core::applyOrder(existing, settings_.sceneOrder))
		out.append(toQ(n));
	return out;
}

QString Vertical::currentScene() const
{
	return toQ(settings_.currentScene);
}

OBSSourceAutoRelease Vertical::sceneSource(const QString &name) const
{
	if (!canvas_ || name.isEmpty())
		return nullptr;
	return obs_canvas_get_source_by_name(canvas_, name.toUtf8().constData());
}

void Vertical::setCurrentScene(const QString &name)
{
	if (!canvas_ || !rawSceneNames().contains(name))
		return;
	if (toStd(name) == settings_.currentScene)
		return;
	settings_.currentScene = toStd(name);
	if (gate_.active())
		attachProgram();
	emit currentSceneChanged();
}

QString Vertical::addScene(const QString &baseName)
{
	if (!canvas_ && !ensureCanvas())
		return {};
	std::vector<std::string> taken;
	for (const QString &n : rawSceneNames())
		taken.push_back(toStd(n));
	const std::string name =
		core::uniqueName(toStd(baseName.trimmed().isEmpty() ? T("Scene.DefaultName") : baseName), taken);
	obs_scene_t *scene = obs_canvas_scene_create(canvas_, name.c_str());
	if (!scene)
		return {};
	registerSceneHotkey(obs_scene_get_source(scene));
	obs_scene_release(scene); /* the canvas keeps its own reference (SCENE_REF) */
	settings_.sceneOrder.push_back(name);
	if (settings_.currentScene.empty())
		setCurrentScene(toQ(name));
	emit scenesChanged();
	return toQ(name);
}

bool Vertical::removeScene(const QString &name)
{
	const QStringList names = sceneNames();
	if (names.size() <= 1 || !names.contains(name))
		return false; /* keep at least one scene so the output never goes black by accident */
	OBSSourceAutoRelease source = sceneSource(name);
	if (!source)
		return false;
	if (currentScene() == name) {
		const int i = names.indexOf(name);
		setCurrentScene(names.value(i == 0 ? 1 : i - 1));
	}
	obs_source_remove(source);
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		settings_.links.removeVertical(toStd(name));
	}
	auto &order = settings_.sceneOrder;
	order.erase(std::remove(order.begin(), order.end(), toStd(name)), order.end());
	emit scenesChanged();
	emit linksChanged();
	return true;
}

bool Vertical::renameScene(const QString &from, const QString &to, QString *error)
{
	const QString clean = to.trimmed();
	if (clean.isEmpty() || clean == from)
		return false;
	if (rawSceneNames().contains(clean)) {
		if (error)
			*error = T("Scene.NameTaken");
		return false;
	}
	OBSSourceAutoRelease source = sceneSource(from);
	if (!source)
		return false;
	/* The source_rename signal updates order, links and the current scene. */
	obs_source_set_name(source, clean.toUtf8().constData());
	return true;
}

void Vertical::moveScene(int from, int to)
{
	std::vector<std::string> order;
	for (const QString &n : sceneNames())
		order.push_back(toStd(n));
	if (core::moveItem(order, from, to)) {
		settings_.sceneOrder = order;
		emit scenesChanged();
	}
}

void Vertical::registerSceneHotkey(obs_source_t *scene)
{
	if (!scene)
		return;
	const QString uuid = toQ(obs_source_get_uuid(scene));
	if (g_hotkeyScenes.contains(uuid))
		return;
	g_hotkeyScenes.insert(uuid);
	/* Registered on the scene itself: OBS saves the binding with the scene and
	 * it survives renames. */
	obs_hotkey_register_source(scene, "vertical.select_scene", obs_module_text("Hotkey.SelectScene"),
				   selectSceneHotkey, nullptr);
}

/* =================================================================== links */

QString Vertical::mainSceneName() const
{
	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	return scene ? toQ(obs_source_get_name(scene)) : QString();
}

void Vertical::setLinkedMode(bool on)
{
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		settings_.linkedScenes = on;
	}
	emit linksChanged();
	if (on)
		onMainSceneChanged();
}

bool Vertical::linkScene(const QString &mainScene, const QString &verticalScene)
{
	bool changed;
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		changed = settings_.links.link(toStd(mainScene), toStd(verticalScene));
	}
	if (changed)
		emit linksChanged();
	return changed;
}

void Vertical::unlinkScene(const QString &mainScene)
{
	bool changed;
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		changed = settings_.links.unlink(toStd(mainScene));
	}
	if (changed)
		emit linksChanged();
}

/* =================================================================== render gate */

void Vertical::acquireRender(core::RenderReason r)
{
	if (gate_.acquire(r)) {
		attachProgram();
		emit renderingChanged(true);
	}
}

void Vertical::releaseRender(core::RenderReason r)
{
	if (gate_.release(r)) {
		detachProgram();
		emit renderingChanged(false);
	}
}

void Vertical::attachProgram()
{
	if (!canvas_)
		return;
	OBSSourceAutoRelease scene = sceneSource(currentScene());
	/* The canvas holds its own reference to the channel source. */
	obs_canvas_set_channel(canvas_, 0, scene);
}

void Vertical::detachProgram()
{
	/* With no source on its channel the canvas mix renders nothing but a clear:
	 * no scene is drawn and its sources are no longer active for this canvas. */
	if (canvas_)
		obs_canvas_set_channel(canvas_, 0, nullptr);
}

/* =================================================================== api */

void Vertical::refreshApiSnapshot()
{
	std::lock_guard<std::mutex> lock(apiMutex_);
	api_ = ApiCanvas();
	if (!canvas_)
		return;
	api_.ready = true;
	api_.uuid = obs_canvas_get_uuid(canvas_);
	api_.name = obs_canvas_get_name(canvas_);
	api_.width = settings_.canvas.size.width;
	api_.height = settings_.canvas.size.height;
}

Vertical::ApiCanvas Vertical::apiCanvas() const
{
	std::lock_guard<std::mutex> lock(apiMutex_);
	return api_;
}

bool Vertical::apiLinkScene(const std::string &mainScene, const std::string &verticalScene, std::string *error)
{
	obs_canvas_t *canvas = nullptr;
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		if (!api_.ready) {
			if (error)
				*error = "vertical canvas not ready";
			return false;
		}
		canvas = obs_get_canvas_by_uuid(api_.uuid.c_str());
	}
	if (!canvas) {
		if (error)
			*error = "vertical canvas not found";
		return false;
	}
	bool ok = true;
	if (!verticalScene.empty()) {
		OBSSourceAutoRelease scene = obs_canvas_get_source_by_name(canvas, verticalScene.c_str());
		if (!scene) {
			ok = false;
			if (error)
				*error = "no vertical scene named '" + verticalScene + "'";
		}
	}
	obs_canvas_release(canvas);
	if (!ok)
		return false;
	if (mainScene.empty()) {
		if (error)
			*error = "main scene name is empty";
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		if (verticalScene.empty())
			settings_.links.unlink(mainScene);
		else
			settings_.links.link(mainScene, verticalScene);
	}
	QMetaObject::invokeMethod(this, [this] { emit linksChanged(); }, Qt::QueuedConnection);
	return true;
}

/* =================================================================== global signals */

void Vertical::connectGlobalSignals()
{
	if (signalsConnected_)
		return;
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_rename", sourceRenameCb, this);
	signal_handler_connect(sh, "source_remove", sourceRemoveCb, this);
	signalsConnected_ = true;
}

void Vertical::disconnectGlobalSignals()
{
	if (!signalsConnected_)
		return;
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_disconnect(sh, "source_rename", sourceRenameCb, this);
	signal_handler_disconnect(sh, "source_remove", sourceRemoveCb, this);
	signalsConnected_ = false;
}

void Vertical::sourceRenameCb(void *data, calldata_t *cd)
{
	auto *self = static_cast<Vertical *>(data);
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_source_is_scene(source))
		return;
	const QString from = toQ(calldata_string(cd, "prev_name"));
	const QString to = toQ(calldata_string(cd, "new_name"));
	OBSSource ref = source;
	QMetaObject::invokeMethod(
		self, [self, ref, from, to] { self->onSourceRenamed(ref, from, to); }, Qt::QueuedConnection);
}

void Vertical::sourceRemoveCb(void *data, calldata_t *cd)
{
	auto *self = static_cast<Vertical *>(data);
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_source_is_scene(source))
		return;
	OBSSource ref = source;
	QMetaObject::invokeMethod(self, [self, ref] { self->onSourceRemoved(ref); }, Qt::QueuedConnection);
}

void Vertical::canvasRemoveCb(void *data, calldata_t *)
{
	auto *self = static_cast<Vertical *>(data);
	QMetaObject::invokeMethod(
		self,
		[self] {
			if (self->canvas_ && obs_canvas_removed(self->canvas_)) {
				self->releaseCanvas();
				emit self->canvasChanged();
				emit self->scenesChanged();
			}
		},
		Qt::QueuedConnection);
}

void Vertical::canvasSourceAddCb(void *data, calldata_t *cd)
{
	auto *self = static_cast<Vertical *>(data);
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_source_is_scene(source))
		return;
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
	QMetaObject::invokeMethod(
		self,
		[self, uuid] {
			OBSSourceAutoRelease scene = obs_get_source_by_uuid(uuid.toUtf8().constData());
			if (!scene || !self->canvas_)
				return;
			self->registerSceneHotkey(scene);
			self->refreshApiSnapshot();
			emit self->scenesChanged();
		},
		Qt::QueuedConnection);
}

void Vertical::onSourceRenamed(obs_source_t *source, const QString &from, const QString &to)
{
	OBSCanvasAutoRelease owner = obs_source_get_canvas(source);
	const bool ours = canvas_ && owner == canvas_;
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		if (ours) {
			settings_.links.renameVertical(toStd(from), toStd(to));
			std::replace(settings_.sceneOrder.begin(), settings_.sceneOrder.end(), toStd(from), toStd(to));
			if (settings_.currentScene == toStd(from))
				settings_.currentScene = toStd(to);
		} else {
			settings_.links.renameMain(toStd(from), toStd(to));
		}
	}
	if (ours) {
		emit scenesChanged();
		emit currentSceneChanged();
	}
	emit linksChanged();
}

void Vertical::onSourceRemoved(obs_source_t *source)
{
	OBSCanvasAutoRelease owner = obs_source_get_canvas(source);
	if (canvas_ && owner == canvas_) {
		emit scenesChanged(); /* removeScene() already cleaned the links */
		return;
	}
	/* During a collection switch every scene is removed: the links were saved
	 * with the old collection, keep them. Otherwise a main scene was deleted. */
	if (switching_ || !loaded_)
		return;
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	if (owner != mainCanvas)
		return;
	bool changed;
	{
		std::lock_guard<std::mutex> lock(apiMutex_);
		changed = settings_.links.unlink(obs_source_get_name(source));
	}
	if (changed)
		emit linksChanged();
}

} // namespace vertical
