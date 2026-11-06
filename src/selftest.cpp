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

/* In-OBS self-test. Compiled only with -DVERTICAL_SELFTEST=ON and run only when
 * the VERTICAL_SELFTEST environment variable is set. It drives the real plugin
 * code (canvas, scenes, render gate, outputs, encoder pool, public procs)
 * against the real libobs / frontend API and logs
 * "[selftest] PASS|FAIL <name> <detail>", "[selftest] PERF ..." and
 * "[selftest] DONE pass=N fail=M". Never part of a release build.
 *
 *   VERTICAL_SELFTEST=1         run it 1.5 s after OBS finished loading
 *   VERTICAL_SELFTEST_QUIT=1    close OBS when done
 *   VERTICAL_SELFTEST_ONLY=a,b  only these groups: canvas, scenes, gate, record, stream
 */

#include "core/destination.hpp"
#include "core/encoders.hpp"
#include "core/recording.hpp"
#include "core/render-gate.hpp"
#include "obs/api.hpp"
#include "obs/hotkeys.hpp"
#include "obs/outputs.hpp"
#include "obs/vertical.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <plugin-support.h>
#include <util/platform.h>

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QImage>
#include <QMainWindow>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace vertical {

namespace {

/* ------------------------------------------------------------------ runner */

struct Step {
	int delayMs = 0;
	std::function<void()> fn;
	/* Polling step: waits until `until` is true (PASS) or the timeout (FAIL). */
	std::function<bool()> until;
	int timeoutMs = 0;
	std::string name;
};

struct Runner {
	std::vector<Step> steps;
	size_t next = 0;
	int pass = 0;
	int fail = 0;
	std::function<void()> onDone; /* drops what the tests hold, while OBS runs */

	void check(bool ok, const char *name, const QString &detail = QString())
	{
		(ok ? pass : fail)++;
		obs_log(ok ? LOG_INFO : LOG_WARNING, "[selftest] %s %s %s", ok ? "PASS" : "FAIL", name,
			detail.toUtf8().constData());
	}
	void add(int delayMs, std::function<void()> fn)
	{
		Step s;
		s.delayMs = delayMs;
		s.fn = std::move(fn);
		steps.push_back(std::move(s));
	}
	void waitUntil(const char *name, int timeoutMs, std::function<bool()> cond)
	{
		Step s;
		s.until = std::move(cond);
		s.timeoutMs = timeoutMs;
		s.name = name;
		steps.push_back(std::move(s));
	}
	void poll(size_t index, uint64_t startNs)
	{
		const Step &s = steps[index];
		const int elapsed = (int)((os_gettime_ns() - startNs) / 1000000);
		if (s.until()) {
			check(true, s.name.c_str(), QStringLiteral("after %1 ms").arg(elapsed));
			run();
			return;
		}
		if (elapsed >= s.timeoutMs) {
			check(false, s.name.c_str(), QStringLiteral("timeout after %1 ms").arg(elapsed));
			run();
			return;
		}
		QTimer::singleShot(100, [this, index, startNs] { poll(index, startNs); });
	}
	void run()
	{
		if (next >= steps.size()) {
			if (onDone)
				onDone();
			obs_log(LOG_INFO, "[selftest] DONE pass=%d fail=%d", pass, fail);
			if (getenv("VERTICAL_SELFTEST_QUIT"))
				QTimer::singleShot(500, [] {
					static_cast<QMainWindow *>(obs_frontend_get_main_window())->close();
				});
			return;
		}
		const size_t index = next++;
		const Step &s = steps[index];
		if (s.until) {
			const uint64_t start = os_gettime_ns();
			QTimer::singleShot(0, [this, index, start] { poll(index, start); });
			return;
		}
		QTimer::singleShot(s.delayMs, [this, fn = s.fn] {
			fn();
			run();
		});
	}
};

Runner *R = nullptr;

/* ------------------------------------------------------------------ state */

const char *kMainName = "Selftest Main";

struct State {
	QString previousScene; /* current vertical scene before the test */
	QString scene;         /* scene made by the scenes group */
	QString recordScene;   /* scene recorded by the gate/record groups */
	QString folder;        /* QDir::temp()/vertical-selftest */
	QString noisePath;     /* noise image shown during the recordings */
	core::RecordingSettings savedRecording;
	bool recordingSaved = false;
	bool holdsPreview = false; /* the scenes group holds the gate (Preview reason) */
	QDockWidget *hiddenDock = nullptr;
	QTimer *mover = nullptr; /* moves the noise image so every frame differs */
	std::string destinationId;
	os_cpu_usage_info_t *cpu = nullptr;
	uint32_t totalFrames = 0;
	uint32_t laggedFrames = 0;
	uint32_t canvasTotal = 0;
	uint32_t canvasSkipped = 0;
	uint64_t recordStartNs = 0;
};

State g;

Vertical &V()
{
	return Vertical::instance();
}

Outputs &O()
{
	return Outputs::instance();
}

QString sizeText(int w, int h)
{
	return QStringLiteral("%1x%2").arg(w).arg(h);
}

/* The source on channel 0 of the vertical canvas (null when nothing is on air). */
OBSSourceAutoRelease channel0()
{
	if (!V().canvas())
		return nullptr;
	return obs_canvas_get_channel(V().canvas(), 0);
}

QString linkOf(const char *mainScene)
{
	const auto v = V().settings().links.verticalFor(mainScene);
	return v ? QString::fromStdString(*v) : QString();
}

std::vector<std::string> outputTypes()
{
	std::vector<std::string> out;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_output_types(i, &id); i++)
		out.emplace_back(id);
	return out;
}

void clearFolder()
{
	QDir dir(g.folder);
	dir.mkpath(QStringLiteral("."));
	for (const QFileInfo &f : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
		QFile::remove(f.absoluteFilePath());
}

QFileInfo newestFile()
{
	QDir dir(g.folder);
	const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Time);
	return files.isEmpty() ? QFileInfo() : files.first();
}

/* Records into the temporary folder; the settings are restored in onDone. */
void useTempRecordFolder()
{
	g.folder = QDir::temp().filePath(QStringLiteral("vertical-selftest"));
	clearFolder();
	if (!g.recordingSaved) {
		g.savedRecording = V().settings().recording;
		g.recordingSaved = true;
	}
	V().settings().recording.path = g.folder.toStdString();
	V().settingsEdited();
}

/* A scene with a random-noise image that moves every frame: the encoder has
 * real work to do and the file size check means something. */
void makeRecordScene()
{
	if (!g.recordScene.isEmpty())
		return;
	if (g.previousScene.isEmpty())
		g.previousScene = V().currentScene();
	g.noisePath = QDir::temp().filePath(QStringLiteral("vertical-selftest-noise.png"));
	if (!QFileInfo::exists(g.noisePath)) {
		QImage img(540, 960, QImage::Format_RGB32);
		std::mt19937 rng(42);
		for (int y = 0; y < img.height(); y++) {
			auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
			for (int x = 0; x < img.width(); x++)
				line[x] = qRgb(rng() & 0xFF, rng() & 0xFF, rng() & 0xFF);
		}
		img.save(g.noisePath);
	}
	g.recordScene = V().addScene(QStringLiteral("Selftest Record"));
	OBSSourceAutoRelease sceneSrc = V().sceneSource(g.recordScene);
	obs_scene_t *scene = obs_scene_from_source(sceneSrc);
	if (!scene)
		return;
	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_string(s, "file", g.noisePath.toUtf8().constData());
	OBSSourceAutoRelease img = obs_source_create_private("image_source", "Selftest Noise", s);
	obs_sceneitem_t *item = obs_scene_add(scene, img);
	vec2 scale;
	vec2_set(&scale, 2.0f, 2.0f);
	obs_sceneitem_set_scale(item, &scale);
	V().setCurrentScene(g.recordScene);

	if (!g.mover) {
		g.mover = new QTimer();
		g.mover->setInterval(33);
		QObject::connect(g.mover, &QTimer::timeout, [] {
			static int t = 0;
			t++;
			OBSSourceAutoRelease src = V().sceneSource(g.recordScene);
			obs_scene_t *sc = obs_scene_from_source(src);
			obs_sceneitem_t *it = sc ? obs_scene_find_source(sc, "Selftest Noise") : nullptr;
			if (!it)
				return;
			vec2 p;
			vec2_set(&p, (float)((t * 7) % 64) - 64.0f, (float)((t * 11) % 64) - 64.0f);
			obs_sceneitem_set_pos(it, &p);
		});
	}
	g.mover->start();
}

void dropRecordScene()
{
	if (g.mover)
		g.mover->stop();
	if (!g.recordScene.isEmpty()) {
		if (!g.previousScene.isEmpty())
			V().setCurrentScene(g.previousScene);
		V().removeScene(g.recordScene);
		g.recordScene.clear();
	}
}

/* Hides the Vertical dock so its preview lets go of the render gate. */
void hideDockPreview()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QDockWidget *dock = main ? main->findChild<QDockWidget *>(QStringLiteral("vertical-dock")) : nullptr;
	if (dock && dock->isVisible()) {
		dock->hide();
		g.hiddenDock = dock;
	}
}

void restoreDock()
{
	if (g.hiddenDock) {
		g.hiddenDock->show();
		g.hiddenDock = nullptr;
	}
}

OBSOutputAutoRelease outputNamed(const char *name)
{
	return obs_get_output_by_name(name);
}

/* ------------------------------------------------------------------ canvas */

struct ProcCanvas {
	bool ready = false;
	QString uuid;
	QString name;
	long long width = 0;
	long long height = 0;
	long long version = 0;
};

ProcCanvas procGetCanvas()
{
	ProcCanvas c;
	calldata_t cd = {};
	if (proc_handler_call(obs_get_proc_handler(), "vertical_get_canvas", &cd)) {
		c.ready = calldata_bool(&cd, "ready");
		c.uuid = QString::fromUtf8(calldata_string(&cd, "uuid"));
		c.name = QString::fromUtf8(calldata_string(&cd, "name"));
		c.width = calldata_int(&cd, "width");
		c.height = calldata_int(&cd, "height");
		c.version = calldata_int(&cd, "api_version");
	}
	calldata_free(&cd);
	return c;
}

bool procLink(const char *mainScene, const QString &verticalScene, QString *error)
{
	calldata_t cd = {};
	calldata_set_string(&cd, "main_scene", mainScene);
	calldata_set_string(&cd, "vertical_scene", verticalScene.toUtf8().constData());
	bool ok = false;
	if (proc_handler_call(obs_get_proc_handler(), "vertical_link_scene", &cd)) {
		ok = calldata_bool(&cd, "success");
		if (error)
			*error = QString::fromUtf8(calldata_string(&cd, "error"));
	}
	calldata_free(&cd);
	return ok;
}

void testCanvas()
{
	Vertical &v = V();
	obs_canvas_t *canvas = v.canvas();
	R->check(canvas && !obs_canvas_removed(canvas), "canvas-exists",
		 canvas ? QString::fromUtf8(obs_canvas_get_name(canvas)) : QStringLiteral("no canvas"));
	if (!canvas)
		return;

	const core::Size want = v.canvasSize();
	const core::Size def = core::defaultCanvasSize();
	obs_video_info ovi = {};
	const bool hasInfo = obs_canvas_get_video_info(canvas, &ovi);
	R->check(hasInfo && (int)ovi.base_width == want.width && (int)ovi.base_height == want.height, "canvas-size",
		 QStringLiteral("canvas %1, configured %2%3")
			 .arg(sizeText((int)ovi.base_width, (int)ovi.base_height), sizeText(want.width, want.height),
			      want == def ? QStringLiteral(" (default)") : QStringLiteral(" (custom)")));
	const core::Size out = v.outputSize();
	R->check(hasInfo && (int)ovi.output_width == out.width && (int)ovi.output_height == out.height,
		 "canvas-output-size", sizeText((int)ovi.output_width, (int)ovi.output_height));
	R->check(obs_canvas_get_video(canvas) != nullptr, "canvas-has-video");

	const ProcCanvas pc = procGetCanvas();
	const QString uuid = QString::fromUtf8(obs_canvas_get_uuid(canvas));
	const QString name = QString::fromUtf8(obs_canvas_get_name(canvas));
	R->check(pc.ready, "proc-get-canvas-ready");
	R->check(pc.uuid == uuid, "proc-get-canvas-uuid", pc.uuid);
	R->check(pc.name == name, "proc-get-canvas-name", pc.name);
	R->check(pc.width == want.width && pc.height == want.height, "proc-get-canvas-size",
		 sizeText((int)pc.width, (int)pc.height));
	R->check(pc.version == api::kApiVersion, "proc-get-canvas-version", QString::number(pc.version));

	const QString target = v.currentScene();
	QString error;
	const bool linked = procLink(kMainName, target, &error);
	R->check(linked && linkOf(kMainName) == target, "proc-link-scene",
		 QStringLiteral("'%1' -> '%2' %3").arg(QString::fromUtf8(kMainName), linkOf(kMainName), error));
	const bool bad = procLink(kMainName, QStringLiteral("No Such Vertical Scene 7f3a"), &error);
	R->check(!bad && !error.isEmpty() && linkOf(kMainName) == target, "proc-link-unknown-scene-refused", error);
	const bool unlinked = procLink(kMainName, QString(), &error);
	R->check(unlinked && linkOf(kMainName).isEmpty(), "proc-unlink-scene", error);
}

/* ------------------------------------------------------------------ scenes */

void scenesAdd()
{
	Vertical &v = V();
	if (g.previousScene.isEmpty())
		g.previousScene = v.currentScene();
	g.scene = v.addScene(QStringLiteral("Selftest Scene"));
	R->check(!g.scene.isEmpty() && v.sceneNames().contains(g.scene), "scene-add", g.scene);
	OBSSourceAutoRelease src = v.sceneSource(g.scene);
	obs_scene_t *scene = obs_scene_from_source(src);
	OBSCanvasAutoRelease owner = src ? obs_source_get_canvas(src) : nullptr;
	R->check(scene && owner == v.canvas(), "scene-in-vertical-canvas");
	if (!scene)
		return;

	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_int(s, "width", 1080);
	obs_data_set_int(s, "height", 1920);
	obs_data_set_int(s, "color", 0xFFE05030);
	OBSSourceAutoRelease color = obs_source_create_private("color_source", "Selftest Color", s);
	obs_sceneitem_t *item = color ? obs_scene_add(scene, color) : nullptr;
	R->check(item != nullptr, "scene-color-source");

	v.setCurrentScene(g.scene);
	R->check(v.currentScene() == g.scene, "scene-set-current", v.currentScene());

	/* Hold the gate like a visible preview does: the scene goes on channel 0. */
	v.acquireRender(core::RenderReason::Preview);
	g.holdsPreview = true;
	OBSSourceAutoRelease ch = channel0();
	R->check(v.rendering() && ch && ch == src, "scene-program-channel",
		 ch ? QString::fromUtf8(obs_source_get_name(ch)) : QStringLiteral("empty"));

	v.linkScene(QString::fromUtf8(kMainName), g.scene);
	R->check(linkOf(kMainName) == g.scene, "scene-link", linkOf(kMainName));
}

void scenesRename()
{
	QString error;
	const bool ok = V().renameScene(g.scene, QStringLiteral("Selftest Renamed"), &error);
	R->check(ok, "scene-rename-call", error);
	/* Taken names are refused. */
	const QStringList names = V().sceneNames();
	QString other;
	for (const QString &n : names)
		if (n != g.scene)
			other = n;
	if (!other.isEmpty()) {
		QString err2;
		const bool dup = V().renameScene(QStringLiteral("Selftest Renamed"), other, &err2);
		R->check(!dup, "scene-rename-taken-refused", err2);
	}
}

void scenesCheckRenamed()
{
	Vertical &v = V();
	const QString renamed = QStringLiteral("Selftest Renamed");
	const QStringList names = v.sceneNames();
	R->check(names.contains(renamed) && !names.contains(g.scene), "scene-rename-list",
		 names.join(QStringLiteral(", ")));
	R->check(v.currentScene() == renamed, "scene-rename-current", v.currentScene());
	R->check(linkOf(kMainName) == renamed, "scene-rename-link-follows", linkOf(kMainName));
	R->check(std::find(v.settings().sceneOrder.begin(), v.settings().sceneOrder.end(), renamed.toStdString()) !=
			 v.settings().sceneOrder.end(),
		 "scene-rename-order-follows");
	OBSSourceAutoRelease src = v.sceneSource(renamed);
	OBSSourceAutoRelease ch = channel0();
	R->check(src && ch == src, "scene-rename-still-on-channel");
	/* The proc sees the new name too. */
	QString error;
	R->check(procLink(kMainName, renamed, &error), "scene-rename-proc-link", error);
	g.scene = renamed;
}

void scenesRemove()
{
	Vertical &v = V();
	v.unlinkScene(QString::fromUtf8(kMainName));
	R->check(linkOf(kMainName).isEmpty(), "scene-unlink");
	v.linkScene(QString::fromUtf8(kMainName), g.scene); /* removal must drop it */
	if (g.holdsPreview) {
		v.releaseRender(core::RenderReason::Preview);
		g.holdsPreview = false;
	}
	const bool ok = v.removeScene(g.scene);
	R->check(ok, "scene-remove-call");
	R->check(!v.sceneNames().contains(g.scene), "scene-remove-list", v.sceneNames().join(QStringLiteral(", ")));
	R->check(v.currentScene() != g.scene && !v.currentScene().isEmpty(), "scene-remove-current-moved",
		 v.currentScene());
	R->check(linkOf(kMainName).isEmpty(), "scene-remove-link-dropped", linkOf(kMainName));
	if (!g.previousScene.isEmpty())
		v.setCurrentScene(g.previousScene);
}

void scenesCheckRemoved()
{
	OBSSourceAutoRelease src = V().sceneSource(g.scene);
	R->check(!src, "scene-remove-source-gone");
	g.scene.clear();
}

/* ------------------------------------------------------------------ render gate */

void gateIdle(const char *name)
{
	OBSSourceAutoRelease ch = channel0();
	R->check(!V().rendering() && !ch, name,
		 QStringLiteral("rendering=%1 channel=%2 outputs=%3")
			 .arg(V().rendering())
			 .arg(ch ? QString::fromUtf8(obs_source_get_name(ch)) : QStringLiteral("empty"))
			 .arg(O().activeCount()));
}

void gateStart()
{
	QString error;
	const bool ok = O().startRecording(&error);
	R->check(ok, "gate-start-recording", error);
	OBSSourceAutoRelease ch = channel0();
	OBSSourceAutoRelease cur = V().sceneSource(V().currentScene());
	R->check(V().rendering() && ch && ch == cur, "gate-active-on-output",
		 ch ? QString::fromUtf8(obs_source_get_name(ch)) : QStringLiteral("empty"));
}

/* ------------------------------------------------------------------ recording */

void recordStart()
{
	const core::RecordingSettings &rs = V().settings().recording;
	const core::MuxerChoice m = core::chooseMuxer(rs.format, outputTypes());
	obs_log(LOG_INFO, "[selftest] INFO record-format %s -> %s .%s%s", core::recordFormatId(rs.format),
		m.outputType.c_str(), m.extension.c_str(), m.fellBack ? " (fallback)" : "");
	clearFolder();

	QString error;
	const bool ok = O().startRecording(&error);
	R->check(ok, "record-start", error);
	g.recordStartNs = os_gettime_ns();
	g.cpu = os_cpu_usage_info_start();
	g.totalFrames = obs_get_total_frames();
	g.laggedFrames = obs_get_lagged_frames();
	video_t *video = V().canvas() ? obs_canvas_get_video(V().canvas()) : nullptr;
	g.canvasTotal = video ? video_output_get_total_frames(video) : 0;
	g.canvasSkipped = video ? video_output_get_skipped_frames(video) : 0;
}

void recordEncoder()
{
	OBSOutputAutoRelease out = outputNamed("Vertical Recording");
	obs_encoder_t *venc = out ? obs_output_get_video_encoder(out) : nullptr;
	if (!venc) {
		R->check(false, "record-encoder", QStringLiteral("no video encoder on the recording"));
		return;
	}
	const char *id = obs_encoder_get_id(venc);
	const char *codec = obs_encoder_get_codec(venc);
	const char *display = obs_encoder_get_display_name(id);
	const core::EncoderCandidate used{id ? id : "", codec ? codec : "", display ? display : ""};
	const bool hardware = core::isHardwareEncoder(used);
	R->check(true, "record-encoder",
		 QStringLiteral("id=%1 codec=%2 name='%3' %4")
			 .arg(QString::fromUtf8(used.id.c_str()), QString::fromUtf8(used.codec.c_str()),
			      QString::fromUtf8(used.displayName.c_str()),
			      hardware ? QStringLiteral("hardware") : QStringLiteral("software")));
	/* Hardware whenever the machine has one; x264 only as the last resort. */
	const core::EncoderChoice expected =
		core::chooseVideoEncoder(O().videoEncoders(), V().settings().recording.encoding.video);
	R->check(used.id == expected.id && (hardware || !expected.hardware), "record-encoder-choice",
		 QStringLiteral("expected %1 (%2)")
			 .arg(QString::fromStdString(expected.id),
			      expected.hardware ? QStringLiteral("hardware") : QStringLiteral("software")));
	obs_encoder_t *aenc = obs_output_get_audio_encoder(out, 0);
	R->check(aenc != nullptr, "record-audio-encoder",
		 aenc ? QString::fromUtf8(obs_encoder_get_id(aenc)) : QStringLiteral("none"));
}

void recordPerf()
{
	const double cpu = g.cpu ? os_cpu_usage_info_query(g.cpu) : -1.0;
	const double frameMs = (double)obs_get_average_frame_time_ns() / 1e6;
	const uint32_t total = obs_get_total_frames() - g.totalFrames;
	const uint32_t lagged = obs_get_lagged_frames() - g.laggedFrames;
	video_t *video = V().canvas() ? obs_canvas_get_video(V().canvas()) : nullptr;
	const uint32_t cTotal = video ? video_output_get_total_frames(video) - g.canvasTotal : 0;
	const uint32_t cSkipped = video ? video_output_get_skipped_frames(video) - g.canvasSkipped : 0;
	OBSOutputAutoRelease out = outputNamed("Vertical Recording");
	const int outFrames = out ? obs_output_get_total_frames(out) : 0;
	const int outDropped = out ? obs_output_get_frames_dropped(out) : 0;
	obs_encoder_t *venc = out ? obs_output_get_video_encoder(out) : nullptr;
	const double secs = (double)(os_gettime_ns() - g.recordStartNs) / 1e9;
	obs_log(LOG_INFO,
		"[selftest] PERF record secs=%.1f cpu=%.1f%% frame_time_ms=%.2f interval_ms=%.2f lagged=%u/%u "
		"canvas_skipped=%u/%u output_frames=%d output_dropped=%d active_fps=%.2f encoder=%s",
		secs, cpu, frameMs, (double)obs_get_frame_interval_ns() / 1e6, lagged, total, cSkipped, cTotal,
		outFrames, outDropped, obs_get_active_fps(), venc ? obs_encoder_get_id(venc) : "none");
	if (g.cpu) {
		os_cpu_usage_info_destroy(g.cpu);
		g.cpu = nullptr;
	}
	R->check(outFrames > 0, "record-frames-encoded", QStringLiteral("%1 frames").arg(outFrames));
}

void recordCheckFile()
{
	const QFileInfo f = newestFile();
	const qint64 size = f.exists() ? f.size() : 0;
	R->check(f.exists() && size > 50 * 1024, "record-file",
		 f.exists() ? QStringLiteral("%1 (%2 KB)").arg(f.absoluteFilePath()).arg(size / 1024)
			    : QStringLiteral("no file in %1").arg(g.folder));
}

void poolStart()
{
	QString e1, e2;
	const bool rec = O().startRecording(&e1);
	const bool rep = O().startReplay(&e2);
	R->check(rec, "pool-start-recording", e1);
	R->check(rep, "pool-start-replay", e2);
}

void poolCheck()
{
	OBSOutputAutoRelease rec = outputNamed("Vertical Recording");
	OBSOutputAutoRelease rep = outputNamed("Vertical Replay Buffer");
	obs_encoder_t *v1 = rec ? obs_output_get_video_encoder(rec) : nullptr;
	obs_encoder_t *v2 = rep ? obs_output_get_video_encoder(rep) : nullptr;
	obs_encoder_t *a1 = rec ? obs_output_get_audio_encoder(rec, 0) : nullptr;
	obs_encoder_t *a2 = rep ? obs_output_get_audio_encoder(rep, 0) : nullptr;
	const QString summary = O().encoderSummary();
	R->check(v1 && v1 == v2, "pool-shared-video-encoder",
		 QStringLiteral("%1 / %2 · %3")
			 .arg(v1 ? QString::fromUtf8(obs_encoder_get_name(v1)) : QStringLiteral("none"),
			      v2 ? QString::fromUtf8(obs_encoder_get_name(v2)) : QStringLiteral("none"), summary));
	R->check(a1 && a1 == a2, "pool-shared-audio-encoder",
		 QStringLiteral("%1 / %2").arg(
			 a1 ? QString::fromUtf8(obs_encoder_get_name(a1)) : QStringLiteral("none"),
			 a2 ? QString::fromUtf8(obs_encoder_get_name(a2)) : QStringLiteral("none")));
}

void poolStop()
{
	O().stopReplay();
	O().stopRecording();
}

/* ------------------------------------------------------------------ streaming */

void streamAdd()
{
	core::Settings &s = V().settings();
	g.destinationId = core::makeId((unsigned long long)QDateTime::currentMSecsSinceEpoch(), s.destinationIds());
	core::Destination d =
		core::makeDestination(core::Platform::CustomRtmp, g.destinationId, "Selftest Unreachable");
	d.server = "rtmp://127.0.0.1:1/live";
	d.streamKey = "test";
	d.enabled = false; /* never picked by "start all" */
	const auto issues = core::validate(d);
	R->check(!core::hasErrors(issues), "stream-destination-valid", QStringLiteral("%1 issues").arg(issues.size()));
	s.destinations.push_back(d);
	V().settingsEdited();
	hotkeys::syncDestinations();
	R->check(s.findDestination(g.destinationId) != nullptr, "stream-destination-added",
		 QString::fromStdString(g.destinationId));

	QString error;
	const bool ok = O().startDestination(g.destinationId, &error);
	/* Either the start fails at once or the connection fails in the output thread. */
	obs_log(LOG_INFO, "[selftest] INFO stream-start returned %s %s", ok ? "true" : "false",
		error.toUtf8().constData());
}

bool streamInError()
{
	const OutputState st = O().destinationStatus(g.destinationId).state;
	return st == OutputState::Error || st == OutputState::Reconnecting;
}

void streamReport()
{
	const OutputStatus st = O().destinationStatus(g.destinationId);
	static const char *names[] = {"Idle", "Starting", "Live", "Reconnecting", "Stopping", "Error"};
	R->check(st.state == OutputState::Error || st.state == OutputState::Reconnecting, "stream-error-state",
		 QStringLiteral("%1: %2").arg(QString::fromUtf8(names[(int)st.state]), st.error));
	O().stopDestination(g.destinationId);
}

void streamRemove()
{
	core::Settings &s = V().settings();
	auto &list = s.destinations;
	list.erase(std::remove_if(list.begin(), list.end(),
				  [](const core::Destination &d) { return d.id == g.destinationId; }),
		   list.end());
	V().settingsEdited();
	hotkeys::syncDestinations();
	R->check(s.findDestination(g.destinationId) == nullptr, "stream-destination-removed");
	g.destinationId.clear();
}

/* ------------------------------------------------------------------ multistream */
/* Real streaming to three local RTMP servers (VERTICAL_SELFTEST_RTMP_PORT = first port of
 * three listeners, e.g. ffmpeg -listen 1): all live, one shared video encoder, CPU. */
std::vector<std::string> g_multi;

void multiStart()
{
	const int base = atoi(getenv("VERTICAL_SELFTEST_RTMP_PORT"));
	core::Settings &s = V().settings();
	for (int i = 0; i < 3; i++) {
		const std::string id =
			core::makeId((unsigned long long)QDateTime::currentMSecsSinceEpoch() + i, s.destinationIds());
		core::Destination d = core::makeDestination(core::Platform::CustomRtmp, id,
							    "Selftest Local " + std::to_string(i + 1));
		d.server = "rtmp://127.0.0.1:" + std::to_string(base + i) + "/live";
		d.streamKey = "test";
		d.enabled = false;
		s.destinations.push_back(d);
		g_multi.push_back(id);
	}
	V().settingsEdited();
	hotkeys::syncDestinations();
	g.cpu = os_cpu_usage_info_start();
	g.totalFrames = obs_get_total_frames();
	g.laggedFrames = obs_get_lagged_frames();
	g.recordStartNs = os_gettime_ns();
	for (const std::string &id : g_multi) {
		QString error;
		const bool ok = O().startDestination(id, &error);
		R->check(ok, "multistream-start", QString::fromStdString(id) + " " + error);
	}
}

bool multiAllLive()
{
	for (const std::string &id : g_multi)
		if (O().destinationStatus(id).state != OutputState::Live)
			return false;
	return !g_multi.empty();
}

void multiCheck()
{
	const double cpu = g.cpu ? os_cpu_usage_info_query(g.cpu) : -1.0;
	if (g.cpu) {
		os_cpu_usage_info_destroy(g.cpu);
		g.cpu = nullptr;
	}
	const uint32_t total = obs_get_total_frames() - g.totalFrames;
	const uint32_t lagged = obs_get_lagged_frames() - g.laggedFrames;
	std::vector<obs_encoder_t *> encoders;
	int dropped = 0, frames = 0;
	for (int i = 0; i < (int)g_multi.size(); i++) {
		OBSOutputAutoRelease out = outputNamed(("Vertical Selftest Local " + std::to_string(i + 1)).c_str());
		if (!out)
			continue;
		encoders.push_back(obs_output_get_video_encoder(out));
		dropped += obs_output_get_frames_dropped(out);
		frames += obs_output_get_total_frames(out);
	}
	const bool shared = encoders.size() == 3 && encoders[0] && encoders[0] == encoders[1] &&
			    encoders[1] == encoders[2];
	obs_log(LOG_INFO,
		"[selftest] PERF multistream secs=%.1f cpu=%.1f%% lagged=%u/%u outputs=%zu frames=%d dropped=%d encoder=%s",
		(double)(os_gettime_ns() - g.recordStartNs) / 1e9, cpu, lagged, total, encoders.size(), frames, dropped,
		encoders.empty() || !encoders[0] ? "none" : obs_encoder_get_id(encoders[0]));
	R->check(shared, "multistream-one-shared-video-encoder", O().encoderSummary());
	R->check(frames > 0 && dropped == 0, "multistream-frames-sent",
		 QStringLiteral("%1 frames, %2 dropped").arg(frames).arg(dropped));
	for (const std::string &id : g_multi)
		O().stopDestination(id);
}

void multiRemove()
{
	core::Settings &s = V().settings();
	auto &list = s.destinations;
	list.erase(std::remove_if(list.begin(), list.end(),
				  [](const core::Destination &d) {
					  return std::find(g_multi.begin(), g_multi.end(), d.id) != g_multi.end();
				  }),
		   list.end());
	V().settingsEdited();
	hotkeys::syncDestinations();
	g_multi.clear();
}

bool enabled(const char *group)
{
	const char *only = getenv("VERTICAL_SELFTEST_ONLY");
	if (!only || !*only)
		return true;
	const QStringList groups = QString::fromUtf8(only).split(QLatin1Char(','), Qt::SkipEmptyParts);
	for (const QString &name : groups)
		if (name.trimmed() == QLatin1String(group))
			return true;
	return false;
}

} // namespace

void runSelfTest()
{
	if (R)
		return;
	R = new Runner();
	R->onDone = [] {
		/* Released here, while libobs runs, not by static destructors. */
		Outputs::instance().stopEverything();
		if (g.holdsPreview) {
			V().releaseRender(core::RenderReason::Preview);
			g.holdsPreview = false;
		}
		dropRecordScene();
		if (!g.scene.isEmpty() && V().sceneNames().contains(g.scene))
			V().removeScene(g.scene);
		if (!g.destinationId.empty())
			streamRemove();
		if (g.recordingSaved) {
			V().settings().recording = g.savedRecording;
			V().settingsEdited();
			g.recordingSaved = false;
		}
		V().unlinkScene(QString::fromUtf8(kMainName));
		restoreDock();
		if (g.cpu) {
			os_cpu_usage_info_destroy(g.cpu);
			g.cpu = nullptr;
		}
		delete g.mover;
		g.mover = nullptr;
		if (!g.noisePath.isEmpty())
			QFile::remove(g.noisePath);
		if (!g.folder.isEmpty())
			obs_log(LOG_INFO, "[selftest] INFO recordings left in %s", g.folder.toUtf8().constData());
	};
	obs_log(LOG_INFO, "[selftest] START");

	if (enabled("canvas")) {
		R->add(0, [] { testCanvas(); });
	}
	if (enabled("scenes")) {
		R->add(200, [] { scenesAdd(); });
		R->add(200, [] { scenesRename(); });
		R->add(600, [] { scenesCheckRenamed(); });
		R->add(200, [] { scenesRemove(); });
		R->add(600, [] { scenesCheckRemoved(); });
	}
	if (enabled("gate")) {
		R->add(200, [] {
			useTempRecordFolder();
			makeRecordScene();
			hideDockPreview();
		});
		R->add(800, [] { gateIdle("gate-idle-without-output"); });
		R->add(0, [] { gateStart(); });
		R->waitUntil("gate-output-live", 5000, [] { return O().recordingStatus().state == OutputState::Live; });
		R->add(500, [] { O().stopRecording(); });
		R->waitUntil("gate-output-stopped", 10000, [] { return !O().recordingActive(); });
		R->add(300, [] { gateIdle("gate-idle-after-stop"); });
		R->add(0, [] { restoreDock(); });
	}
	if (enabled("record")) {
		R->add(200, [] {
			useTempRecordFolder();
			makeRecordScene();
		});
		R->add(300, [] { recordStart(); });
		R->waitUntil("record-live", 5000, [] { return O().recordingStatus().state == OutputState::Live; });
		R->add(0, [] { recordEncoder(); });
		R->add(3000, [] { recordPerf(); });
		R->add(0, [] { O().stopRecording(); });
		R->waitUntil("record-stopped", 15000, [] { return !O().recordingActive(); });
		R->add(500, [] { recordCheckFile(); });
		R->add(300, [] { poolStart(); });
		R->waitUntil("pool-both-live", 5000,
			     [] { return O().recordingStatus().state == OutputState::Live && O().replayActive(); });
		R->add(500, [] { poolCheck(); });
		R->add(0, [] { poolStop(); });
		R->waitUntil("pool-stopped", 15000, [] { return !O().recordingActive() && !O().replayActive(); });
		R->add(200, [] { dropRecordScene(); });
	}
	if (getenv("VERTICAL_SELFTEST_RTMP_PORT") && enabled("multistream")) {
		R->add(300, [] { makeRecordScene(); });
		R->add(500, [] { multiStart(); });
		R->waitUntil("multistream-all-live", 10000, [] { return multiAllLive(); });
		R->add(5000, [] { multiCheck(); });
		R->waitUntil("multistream-stopped", 15000, [] { return !O().anyStreamActive(); });
		R->add(200, [] { multiRemove(); });
		R->add(200, [] { dropRecordScene(); });
	}
	if (enabled("stream")) {
		R->add(300, [] { streamAdd(); });
		R->waitUntil("stream-reports-error", 8000, [] { return streamInError(); });
		R->add(0, [] { streamReport(); });
		R->waitUntil("stream-stopped", 10000, [] { return !O().anyStreamActive(); });
		R->add(200, [] { streamRemove(); });
	}
	R->run();
}

} // namespace vertical
