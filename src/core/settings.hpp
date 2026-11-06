#pragma once

/* Everything the plugin saves with a scene collection, and its JSON form.
 * Pure C++17. Reading is tolerant: unknown keys are ignored, missing or
 * invalid values fall back to defaults and are reported as warnings. */

#include "canvas-size.hpp"
#include "audio-mix.hpp"
#include "destination.hpp"
#include "json.hpp"
#include "recording.hpp"
#include "scene-links.hpp"

#include <string>
#include <vector>

namespace vertical::core {

/* 2: audio (vertical track, exclusions); encodings on track 0 = the vertical mix. */
constexpr int kSettingsVersion = 2;

struct CanvasSettings {
	std::string name = "Vertical";
	std::string uuid; /* canvas uuid once created; empty before */
	Size size = defaultCanvasSize();
	int outputHeight = 0; /* 0 = no downscale */
};

struct AudioSettings {
	int track = 0; /* vertical track last used (1..6), 0 before the first choice */
	AudioExclusions exclusions;
};

struct Settings {
	int version = kSettingsVersion;
	CanvasSettings canvas;
	std::vector<std::string> sceneOrder;
	std::string currentScene;
	bool linkedScenes = false;
	SceneLinks links;
	std::vector<Destination> destinations;
	RecordingSettings recording;
	ReplaySettings replay;
	bool previewVisible = true;
	AudioSettings audio;

	const Destination *findDestination(const std::string &id) const;
	Destination *findDestination(const std::string &id);
	std::vector<std::string> destinationIds() const;
};

enum class KeyMode {
	Include, /* collection save data: the keys are needed to stream */
	Redact,  /* logs, diagnostics, exports shared with support */
};

Json destinationToJson(const Destination &d, KeyMode keys);
Destination destinationFromJson(const Json &j, std::vector<std::string> *warnings, int version = kSettingsVersion);

Json encodingToJson(const EncoderSettings &e);
EncoderSettings encodingFromJson(const Json &j, int version = kSettingsVersion);

Json settingsToJson(const Settings &s, KeyMode keys = KeyMode::Include);
Settings settingsFromJson(const Json &j, std::vector<std::string> *warnings = nullptr);

/* Convenience wrappers on text. An empty or broken text gives the defaults. */
std::string saveSettings(const Settings &s, KeyMode keys = KeyMode::Include, bool pretty = false);
Settings loadSettings(const std::string &text, std::vector<std::string> *warnings = nullptr);

} // namespace vertical::core
