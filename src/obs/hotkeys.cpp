#include "obs/hotkeys.hpp"

#include "obs/outputs.hpp"
#include "obs/text.hpp"
#include "obs/vertical.hpp"

#include <obs.hpp>

#include <QMetaObject>

#include <functional>
#include <map>
#include <string>

namespace vertical::hotkeys {

namespace {

const char *kSaveKey = "vertical_hotkeys";

struct Pair {
	obs_hotkey_pair_id id = OBS_INVALID_HOTKEY_PAIR_ID;
	std::string name0;
	std::string name1;
	std::string *data = nullptr; /* destination id given to OBS as callback data */
};

void dropPair(Pair &p)
{
	if (p.id != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(p.id);
	delete p.data;
	p = Pair();
}

std::map<std::string, Pair> g_static; /* key -> pair */
obs_hotkey_id g_saveReplay = OBS_INVALID_HOTKEY_ID;
std::map<std::string, Pair> g_destinations; /* destination id -> pair */
OBSDataAutoRelease g_bindings;              /* last loaded bindings, for late registrations */

void onUi(std::function<void()> fn)
{
	QMetaObject::invokeMethod(&Outputs::instance(), std::move(fn), Qt::QueuedConnection);
}

void loadPair(const Pair &p)
{
	if (!g_bindings || p.id == OBS_INVALID_HOTKEY_PAIR_ID)
		return;
	OBSDataArrayAutoRelease a0 = obs_data_get_array(g_bindings, p.name0.c_str());
	OBSDataArrayAutoRelease a1 = obs_data_get_array(g_bindings, p.name1.c_str());
	obs_hotkey_pair_load(p.id, a0, a1);
}

void savePair(obs_data_t *out, const Pair &p)
{
	if (p.id == OBS_INVALID_HOTKEY_PAIR_ID)
		return;
	obs_data_array_t *a0 = nullptr;
	obs_data_array_t *a1 = nullptr;
	obs_hotkey_pair_save(p.id, &a0, &a1);
	obs_data_set_array(out, p.name0.c_str(), a0);
	obs_data_set_array(out, p.name1.c_str(), a1);
	obs_data_array_release(a0);
	obs_data_array_release(a1);
}

/* Pair callbacks return true when they acted, so OBS can switch to the partner. */
Pair registerPair(const std::string &base, const QString &desc0, const QString &desc1, obs_hotkey_active_func f0,
		  obs_hotkey_active_func f1, void *data)
{
	Pair p;
	p.name0 = base + ".start";
	p.name1 = base + ".stop";
	p.id = obs_hotkey_pair_register_frontend(p.name0.c_str(), desc0.toUtf8().constData(), p.name1.c_str(),
						 desc1.toUtf8().constData(), f0, f1, data, data);
	return p;
}

/* data of destination pairs: a heap copy of the destination id (Pair::data). */
bool destStart(void *data, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return false;
	const std::string id = *static_cast<std::string *>(data);
	/* Runs on the hotkey thread: all state checks happen on the UI thread. */
	onUi([id] {
		QString error;
		Outputs::instance().startDestination(id, &error);
	});
	return true;
}

bool destStop(void *data, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return false;
	const std::string id = *static_cast<std::string *>(data);
	onUi([id] { Outputs::instance().stopDestination(id); });
	return true;
}

} // namespace

void registerStatic()
{
	g_static["streams"] = registerPair(
		"vertical.streams", T("Hotkey.StartAllStreams"), T("Hotkey.StopAllStreams"),
		[](void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				onUi([] { Outputs::instance().startAll(); });
			return pressed;
		},
		[](void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				onUi([] { Outputs::instance().stopAllStreams(); });
			return pressed;
		},
		nullptr);
	g_static["record"] = registerPair(
		"vertical.record", T("Hotkey.StartRecording"), T("Hotkey.StopRecording"),
		[](void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				onUi([] {
					QString e;
					Outputs::instance().startRecording(&e);
				});
			return pressed;
		},
		[](void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				onUi([] { Outputs::instance().stopRecording(); });
			return pressed;
		},
		nullptr);
	g_static["replay"] = registerPair(
		"vertical.replay", T("Hotkey.StartReplay"), T("Hotkey.StopReplay"),
		[](void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				onUi([] {
					QString e;
					Outputs::instance().startReplay(&e);
				});
			return pressed;
		},
		[](void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				onUi([] { Outputs::instance().stopReplay(); });
			return pressed;
		},
		nullptr);
	g_saveReplay = obs_hotkey_register_frontend(
		"vertical.replay.save", T("Hotkey.SaveReplay").toUtf8().constData(),
		[](void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				onUi([] { Outputs::instance().saveReplay(); });
		},
		nullptr);
}

void syncDestinations()
{
	const core::Settings &s = Vertical::instance().settings();
	/* Drop pairs of removed destinations. */
	for (auto it = g_destinations.begin(); it != g_destinations.end();) {
		if (!s.findDestination(it->first)) {
			dropPair(it->second);
			it = g_destinations.erase(it);
		} else {
			++it;
		}
	}
	for (const core::Destination &d : s.destinations) {
		const QString name = QString::fromStdString(d.name);
		auto it = g_destinations.find(d.id);
		if (it != g_destinations.end()) {
			obs_hotkey_pair_set_descriptions(it->second.id,
							 T("Hotkey.StartDestination").arg(name).toUtf8().constData(),
							 T("Hotkey.StopDestination").arg(name).toUtf8().constData());
			continue;
		}
		auto *id = new std::string(d.id);
		Pair p = registerPair("vertical.dest." + d.id, T("Hotkey.StartDestination").arg(name),
				      T("Hotkey.StopDestination").arg(name), destStart, destStop, id);
		p.data = id;
		loadPair(p);
		g_destinations[d.id] = p;
	}
}

void load(obs_data_t *collectionData)
{
	g_bindings = obs_data_get_obj(collectionData, kSaveKey);
	for (const auto &[k, p] : g_static)
		loadPair(p);
	if (g_bindings && g_saveReplay != OBS_INVALID_HOTKEY_ID) {
		OBSDataArrayAutoRelease a = obs_data_get_array(g_bindings, "vertical.replay.save");
		obs_hotkey_load(g_saveReplay, a);
	}
	/* Destinations of the new collection: forget the old pairs first. */
	for (auto &[id, p] : g_destinations)
		dropPair(p);
	g_destinations.clear();
	syncDestinations();
}

void save(obs_data_t *collectionData)
{
	OBSDataAutoRelease out = obs_data_create();
	for (const auto &[k, p] : g_static)
		savePair(out, p);
	for (const auto &[id, p] : g_destinations)
		savePair(out, p);
	if (g_saveReplay != OBS_INVALID_HOTKEY_ID) {
		OBSDataArrayAutoRelease a = obs_hotkey_save(g_saveReplay);
		obs_data_set_array(out, "vertical.replay.save", a);
	}
	obs_data_set_obj(collectionData, kSaveKey, out);
}

void unregisterAll()
{
	for (auto &[k, p] : g_static)
		obs_hotkey_pair_unregister(p.id);
	g_static.clear();
	for (auto &[id, p] : g_destinations)
		dropPair(p);
	g_destinations.clear();
	if (g_saveReplay != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_saveReplay);
	g_saveReplay = OBS_INVALID_HOTKEY_ID;
	g_bindings = nullptr;
}

} // namespace vertical::hotkeys
