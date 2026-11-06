/*
Vertical for OBS
Copyright (C) 2026 The OnAir Company

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QTimer>

#include <cstdlib>

#include "obs/api.hpp"
#include "obs/audio-mix.hpp"
#include "obs/av-sync.hpp"
#include "obs/hotkeys.hpp"
#include "obs/outputs.hpp"
#include "obs/sync-test.hpp"
#include "obs/vertical.hpp"
#include "obs/virtualaudio.hpp"
#ifdef VERTICAL_VIRTUALCAM
#include "obs/virtualcam.hpp"
#endif
#include "ui/dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Vertical for OBS";
}

#ifdef VERTICAL_SELFTEST
namespace vertical {
void runSelfTest();
}
#endif

namespace {

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	vertical::Vertical &v = vertical::Vertical::instance();
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		v.onMainSceneChanged();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		/* Outputs hold encoders on the canvas of the old collection. */
		vertical::Outputs::instance().stopEverything();
		v.onCollectionChanging();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		v.onCollectionCleanup();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		v.onCollectionReady();
		vertical::hotkeys::syncDestinations();
		vertical::AudioMix::instance().refresh();
		if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
			vertical::avsync::update();
			vertical::virtualaudio::start();
			if (getenv("VERTICAL_SYNC_REMOTE"))
				vertical::synctest::setRemoteTrigger(true);
		}
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
		/* OBS's own outputs may now use the vertical track: choose again. */
		vertical::AudioMix::instance().refresh();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		vertical::virtualaudio::shutdown();
		vertical::synctest::shutdown();
		vertical::AudioMix::instance().shutdown();
		vertical::Outputs::instance().stopEverything();
		vertical::hotkeys::unregisterAll();
		v.shutdown();
		break;
	default:
		break;
	}
#ifdef VERTICAL_SELFTEST
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && getenv("VERTICAL_SELFTEST"))
		QTimer::singleShot(1500, [] { vertical::runSelfTest(); });
#endif
}

void onSave(obs_data_t *data, bool saving, void *)
{
	if (saving) {
		vertical::Vertical::instance().save(data);
		vertical::hotkeys::save(data);
	} else {
		vertical::Vertical::instance().load(data);
		vertical::hotkeys::load(data);
	}
}

} // namespace

bool obs_module_load(void)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	vertical::Vertical::instance().start();
	vertical::AudioMix::instance().start();
	vertical::synctest::registerSources();
	QObject::connect(&vertical::AudioMix::instance(), &vertical::AudioMix::trackChanged,
			 [](int) { vertical::virtualaudio::trackChanged(); });
	vertical::api::registerProcs();
#ifdef VERTICAL_VIRTUALCAM
	vertical::virtualcam::registerOutput();
#endif
	vertical::hotkeys::registerStatic();

	obs_frontend_add_dock_by_id("vertical-dock", obs_module_text("Dock.Title"), new vertical::ui::Dock(main));

	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	obs_frontend_add_save_callback(onSave, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	vertical::api::shutdown();
	obs_frontend_remove_save_callback(onSave, nullptr);
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
