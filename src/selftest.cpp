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
 *   VERTICAL_SELFTEST_ONLY=a,b  only these groups: canvas, scenes, gate, record, multistream,
 *                               stream, dock, audio, vaudio, audioshots, measure (multistream
 *                               needs VERTICAL_SELFTEST_RTMP_PORT; audioshots
 *                               VERTICAL_SELFTEST_SHOTS; measure runs only when named)
 *   VERTICAL_SELFTEST_SHOTS=dir screenshots of the dock in Studio and Compact into dir
 *                               (needs VERTICAL_SELFTEST_RTMP_PORT: two more listeners at +3, +4)
 */

#include "core/destination.hpp"
#include "core/encoders.hpp"
#include "core/recording.hpp"
#include "core/render-gate.hpp"
#include "obs/api.hpp"
#include "obs/audio-mix.hpp"
#include "obs/av-sync.hpp"
#include "obs/hotkeys.hpp"
#include "obs/outputs.hpp"
#include "obs/sync-test.hpp"
#include "obs/text.hpp"
#include "obs/vertical.hpp"
#include "obs/virtualaudio.hpp"
#include "ui/destination-dialog.hpp"
#include "ui/dock-rows.hpp"
#include "ui/dock-style.hpp"
#include "ui/dock.hpp"
#include "ui/mix-meter.hpp"
#include "ui/settings-dialog.hpp"
#include "ui/preview.hpp"

#include <graphics/graphics.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <plugin-support.h>
#include <util/platform.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSpinBox>
#include <QTabWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <memory>
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

/* ------------------------------------------------------------------ dock (0.2) */
/* The redesigned dock: both layouts, the switch by width, a click on a scene row,
 * the link menu, the recording tab, and destination rows during real streams. */

struct DockState {
	bool saved = false;
	bool wasFloating = false;
	bool wasVisible = false;
	QRect geometry;
	core::LayoutChoice choice = core::LayoutChoice::Automatic;
	QString sceneA, sceneB;
	std::string mainScene;
	QString previousLink; /* vertical scene the main scene was linked to before */
	bool createdMain = false;
	QPointer<QMenu> menu;
	QString previousProgram; /* main program scene before the linked switch test */
	bool wasLinkedMode = false;
	std::vector<std::string> shotDestinations;
	std::vector<std::string> rowDestinations; /* destination rows group: ids to remove */
	QStringList shotScenes;
	QString shotImage;
};
DockState gd;

QDockWidget *dockFrame()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	return main ? main->findChild<QDockWidget *>(QStringLiteral("vertical-dock")) : nullptr;
}

ui::Dock *dockWidget()
{
	QDockWidget *d = dockFrame();
	return d ? d->findChild<ui::Dock *>() : nullptr;
}

template<typename W> W *dockChild(const char *name)
{
	ui::Dock *d = dockWidget();
	return d ? d->findChild<W *>(QString::fromUtf8(name)) : nullptr;
}

bool shown(QWidget *w)
{
	return w && w->isVisible();
}

/* Floats the dock at a given size (the real resize path of the dock). */
void dockFloat(int w, int h)
{
	QDockWidget *d = dockFrame();
	if (!d)
		return;
	if (!gd.saved) {
		gd.saved = true;
		gd.wasFloating = d->isFloating();
		gd.wasVisible = d->isVisible();
		gd.geometry = d->geometry();
		if (ui::Dock *dock = dockWidget())
			gd.choice = dock->layoutChoice();
	}
	d->show();
	d->setFloating(true);
	d->resize(w, h);
	d->raise();
}

void dockRestore()
{
	QDockWidget *d = dockFrame();
	if (!d || !gd.saved)
		return;
	if (ui::Dock *dock = dockWidget())
		dock->setLayoutChoice(gd.choice);
	d->setFloating(gd.wasFloating);
	if (gd.wasFloating)
		d->setGeometry(gd.geometry);
	d->setVisible(gd.wasVisible);
	gd.saved = false;
}

void clickAt(QWidget *w, const QPoint &pos)
{
	const QPointF global = w->mapToGlobal(QPointF(pos));
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), global, Qt::LeftButton, Qt::LeftButton,
			  Qt::NoModifier);
	QApplication::sendEvent(w, &press);
	QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos), global, Qt::LeftButton, Qt::NoButton,
			    Qt::NoModifier);
	QApplication::sendEvent(w, &release);
}

QString modeName(core::DockMode m)
{
	return m == core::DockMode::Studio ? QStringLiteral("Studio") : QStringLiteral("Compact");
}

QString widthText()
{
	ui::Dock *d = dockWidget();
	return d ? QStringLiteral("dock %1 px, %2").arg(d->width()).arg(modeName(d->mode()))
		 : QStringLiteral("no dock");
}

void dockCheckStudio()
{
	ui::Dock *d = dockWidget();
	R->check(d && d->mode() == core::DockMode::Studio, "dock-studio-mode", widthText());
	if (!d)
		return;
	ui::Chip *chip = dockChild<ui::Chip>("canvasChip");
	const core::Size cs = V().canvasSize();
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const QString want = QString::fromStdString(core::canvasChip(cs.width, cs.height, ovi.fps_num, ovi.fps_den));
	R->check(chip && shown(chip) && chip->text() == want, "dock-studio-header-chip",
		 chip ? chip->text() : QString());
	R->check(shown(dockChild<QWidget>("linkedSwitch")) && shown(dockChild<QWidget>("dockMore")),
		 "dock-studio-header-switch-and-more");
	R->check(shown(dockChild<QWidget>("previewFrame")), "dock-studio-preview");
	R->check(shown(dockChild<QWidget>("verticalScenes")) && shown(dockChild<QWidget>("addScene")),
		 "dock-studio-scenes-section");
	R->check(shown(dockChild<QWidget>("startAll")) && shown(dockChild<QWidget>("stopAll")),
		 "dock-studio-start-stop-all");
	R->check(shown(dockChild<QWidget>("destinations")) && shown(dockChild<QWidget>("addDestination")),
		 "dock-studio-destinations");
	const bool tabs = shown(dockChild<QWidget>("tab0")) && shown(dockChild<QWidget>("tab1")) &&
			  shown(dockChild<QWidget>("recordButton"));
#ifdef VERTICAL_VIRTUALCAM
	const bool camTab = shown(dockChild<QWidget>("tab2"));
#else
	const bool camTab = true;
#endif
	R->check(tabs && camTab && !shown(dockChild<QWidget>("recLine")), "dock-studio-tabs");
	QLabel *outputs = dockChild<QLabel>("footerOutputs");
	QLabel *cpu = dockChild<QLabel>("footerCpu");
	R->check(!dockChild<QWidget>("encoderChip") && shown(outputs) && shown(cpu) &&
			 shown(dockChild<QWidget>("footerSettings")),
		 "dock-studio-footer-no-encoder-chip",
		 (outputs ? outputs->text() : QString()) + QStringLiteral(" | ") + (cpu ? cpu->text() : QString()));
	auto *scenes = dockChild<ui::SceneList>("verticalScenes");
	R->check(scenes && scenes->rowHeight() == ui::tokens::kRowStudio, "dock-studio-row-height",
		 scenes ? QString::number(scenes->rowHeight()) : QString());
}

void dockCheckCompact()
{
	ui::Dock *d = dockWidget();
	R->check(d && d->mode() == core::DockMode::Compact, "dock-compact-mode", widthText());
	if (!d)
		return;
	R->check(shown(dockChild<QWidget>("previewFrame")) && shown(dockChild<QWidget>("verticalScenes")),
		 "dock-compact-preview-and-scenes");
	R->check(!shown(dockChild<QWidget>("addScene")) && !shown(dockChild<QWidget>("startAll")),
		 "dock-compact-no-section-headers");
	R->check(shown(dockChild<QWidget>("destinations")), "dock-compact-destinations");
	R->check(shown(dockChild<QWidget>("recLine")) && shown(dockChild<QWidget>("replayLine")) &&
			 !shown(dockChild<QWidget>("recordButton")),
		 "dock-compact-lines");
	R->check(!dockChild<QWidget>("encoderChip") && shown(dockChild<QWidget>("footerOutputs")) &&
			 shown(dockChild<QWidget>("footerCpu")) && shown(dockChild<QWidget>("footerSettings")),
		 "dock-compact-footer-no-encoder-chip");
	auto *scenes = dockChild<ui::SceneList>("verticalScenes");
	R->check(scenes && scenes->rowHeight() == ui::tokens::kRowCompact, "dock-compact-row-height",
		 scenes ? QString::number(scenes->rowHeight()) : QString());
}

void dockScenesSetup()
{
	gd.sceneA = V().addScene(QStringLiteral("Selftest Dock A"));
	gd.sceneB = V().addScene(QStringLiteral("Selftest Dock B"));
	if (g.previousScene.isEmpty())
		g.previousScene = V().currentScene();
	V().setCurrentScene(gd.sceneA);
	/* A 16:9 scene to link to: the first main scene, or one made for the test. */
	char **names = obs_frontend_get_scene_names();
	if (names && names[0])
		gd.mainScene = names[0];
	bfree(names);
	if (gd.mainScene.empty()) {
		obs_scene_release(obs_scene_create("Selftest Main 16:9"));
		gd.mainScene = "Selftest Main 16:9";
		gd.createdMain = true;
	}
	gd.previousLink = linkOf(gd.mainScene.c_str());
}

void dockClickScene()
{
	auto *list = dockChild<ui::SceneList>("verticalScenes");
	QListWidgetItem *item = list ? list->itemNamed(gd.sceneB) : nullptr;
	if (!item) {
		R->check(false, "dock-scene-row-click", QStringLiteral("row not found"));
		return;
	}
	list->scrollToItem(item);
	const QRect r = list->visualItemRect(item);
	clickAt(list->viewport(), QPoint(r.left() + r.width() / 3, r.center().y()));
}

void dockCheckSceneOnProgram()
{
	auto *list = dockChild<ui::SceneList>("verticalScenes");
	QListWidgetItem *item = list ? list->itemNamed(gd.sceneB) : nullptr;
	OBSSourceAutoRelease src = V().sceneSource(gd.sceneB);
	OBSSourceAutoRelease ch = channel0();
	R->check(item && item->data(ui::SceneList::ProgramRole).toBool() && (!V().rendering() || ch == src),
		 "dock-scene-row-program", V().currentScene());
}

void dockOpenLinkMenu()
{
	auto *list = dockChild<ui::SceneList>("verticalScenes");
	const QRect lr = list ? list->linkRectFor(gd.sceneB) : QRect();
	if (lr.isEmpty()) {
		R->check(false, "dock-link-menu-open", QStringLiteral("no link icon"));
		return;
	}
	clickAt(list->viewport(), lr.center());
}

QMenu *openLinkMenu()
{
	QWidget *popup = QApplication::activePopupWidget();
	auto *menu = qobject_cast<QMenu *>(popup);
	if (menu && menu->objectName() == QLatin1String("verticalLinkMenu"))
		return menu;
	return nullptr;
}

void dockCloseMenu()
{
	if (gd.menu)
		gd.menu->close();
	if (QMenu *m = openLinkMenu())
		m->close();
}

QAction *linkAction(QMenu *menu)
{
	if (!menu)
		return nullptr;
	for (QAction *a : menu->actions())
		if (a->data().toString() == QString::fromStdString(gd.mainScene))
			return a;
	return nullptr;
}

/* A link menu of scene B opened straight from the dock (the same non-blocking popup
 * the link icon opens), used at once: no event runs between opening it and triggering
 * its entry, so a focus change of the test OBS (which closes popups) cannot get in the
 * way. Opening from a click is checked by "dock-link-menu-open". */
QMenu *freshLinkMenu()
{
	dockCloseMenu();
	ui::Dock *d = dockWidget();
	auto *list = dockChild<ui::SceneList>("verticalScenes");
	if (!d || !list)
		return nullptr;
	const QRect lr = list->linkRectFor(gd.sceneB);
	gd.menu = d->openLinkMenu(gd.sceneB, list->viewport()->mapToGlobal(lr.center()));
	return gd.menu;
}

void dockLinkToggle(bool expectLinked)
{
	QMenu *menu = freshLinkMenu();
	QAction *a = linkAction(menu);
	const char *name = expectLinked ? "dock-link-menu-links" : "dock-link-menu-unlinks";
	if (!a) {
		R->check(false, name, QStringLiteral("no entry for '%1'").arg(QString::fromStdString(gd.mainScene)));
		return;
	}
	const bool before = a->isChecked();
	a->trigger();
	const QString now = linkOf(gd.mainScene.c_str());
	auto *list = dockChild<ui::SceneList>("verticalScenes");
	QListWidgetItem *item = list ? list->itemNamed(gd.sceneB) : nullptr;
	const bool rowLinked = item && item->data(ui::SceneList::LinkedRole).toBool();
	const bool ok = before != expectLinked && (expectLinked ? (now == gd.sceneB && rowLinked && a->isChecked())
								: (now.isEmpty() && !rowLinked && !a->isChecked()));
	R->check(ok, name,
		 QStringLiteral("'%1' -> '%2', checked before %3, row linked %4")
			 .arg(QString::fromStdString(gd.mainScene), now)
			 .arg(before)
			 .arg(rowLinked));
	/* A menu opened again shows the new state. */
	QAction *again = linkAction(freshLinkMenu());
	R->check(again && again->isChecked() == expectLinked,
		 expectLinked ? "dock-link-menu-shows-linked" : "dock-link-menu-shows-unlinked");
	dockCloseMenu();
}

/* With "Linked scenes" on, putting the linked 16:9 scene on program switches the
 * vertical output to scene B; unlinked, it stays where it is. */
void dockLinkedSwitchSetup()
{
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	gd.previousProgram = current ? QString::fromUtf8(obs_source_get_name(current)) : QString();
	obs_scene_release(obs_scene_create("Selftest Link Main"));
	gd.wasLinkedMode = V().linkedMode();
	V().setLinkedMode(true);
	V().setCurrentScene(gd.sceneA);
	R->check(V().linkScene(QStringLiteral("Selftest Link Main"), gd.sceneB), "dock-linked-switch-link");
	OBSSourceAutoRelease linkMain = obs_get_source_by_name("Selftest Link Main");
	obs_frontend_set_current_scene(linkMain);
}

void dockLinkedSwitchUnlinked()
{
	/* Unlinked: leave the test scene (back to the previous program) ... */
	V().unlinkScene(QStringLiteral("Selftest Link Main"));
	OBSSourceAutoRelease prev = obs_get_source_by_name(gd.previousProgram.toUtf8().constData());
	if (prev)
		obs_frontend_set_current_scene(prev);
}

void dockLinkedSwitchAgain()
{
	/* ... and put it on program again: the vertical scene does not move. */
	V().setCurrentScene(gd.sceneA);
	OBSSourceAutoRelease linkMain = obs_get_source_by_name("Selftest Link Main");
	obs_frontend_set_current_scene(linkMain);
}

void dockLinkedSwitchCleanup()
{
	R->check(V().currentScene() == gd.sceneA, "dock-unlinked-switch-keeps-scene", V().currentScene());
	V().setLinkedMode(gd.wasLinkedMode);
	OBSSourceAutoRelease prev = obs_get_source_by_name(gd.previousProgram.toUtf8().constData());
	if (prev)
		obs_frontend_set_current_scene(prev);
	OBSSourceAutoRelease linkMain = obs_get_source_by_name("Selftest Link Main");
	if (linkMain)
		obs_source_remove(linkMain);
}

void dockRecordStart()
{
	if (auto *tab = dockChild<QAbstractButton>("tab0"))
		tab->click();
	auto *b = dockChild<QPushButton>("recordButton");
	R->check(b && shown(b) && b->text() == T("Dock.StartRecording"), "dock-record-button-idle",
		 b ? b->text() : QString());
	if (b)
		b->click();
}

void dockRecordCheckRunning()
{
	auto *b = dockChild<QPushButton>("recordButton");
	auto *timer = dockChild<QLabel>("recordTimer");
	R->check(b && b->text() == T("Dock.StopRecording") && timer && timer->text() != QLatin1String("00:00:00"),
		 "dock-record-tab-running", timer ? timer->text() : QString());
	/* The Compact line shows the same recording. */
	if (ui::Dock *d = dockWidget())
		d->setLayoutChoice(core::LayoutChoice::Compact);
	auto *line = dockChild<ui::ElidedLabel>("recLine");
	R->check(line && shown(line) && line->fullText().startsWith(QStringLiteral("REC")), "dock-record-compact-line",
		 line ? line->fullText() : QString());
	if (ui::Dock *d = dockWidget())
		d->setLayoutChoice(core::LayoutChoice::Studio);
	if (b)
		b->click();
}

void dockRecordCheckStopped()
{
	auto *b = dockChild<QPushButton>("recordButton");
	R->check(b && b->text() == T("Dock.StartRecording") && !O().recordingActive(), "dock-record-tab-stopped",
		 b ? b->text() : QString());
}

void dockPreviewChecks()
{
	ui::Dock *d = dockWidget();
	auto *frame = dockChild<ui::PreviewFrame>("previewFrame");
	if (!d || !frame) {
		R->check(false, "dock-preview-collapse");
		return;
	}
	frame->setCollapsed(true);
	const bool hidden = !frame->preview()->isVisible() && !V().settings().previewVisible &&
			    shown(frame->expandBar());
	frame->setCollapsed(false);
	R->check(hidden && frame->preview()->isVisible() && V().settings().previewVisible, "dock-preview-collapse");
	d->openPreviewWindow();
	ui::PreviewWindow *w = d->previewWindow();
	R->check(w && w->isVisible(), "dock-preview-window-opens");
}

void dockPreviewWindowCheck()
{
	ui::Dock *d = dockWidget();
	ui::PreviewWindow *w = d ? d->previewWindow() : nullptr;
	R->check(w && w->preview()->hasDisplay(), "dock-preview-window-display");
	if (w)
		w->close();
}

void dockCleanup()
{
	dockCloseMenu();
	Vertical &v = V();
	v.unlinkScene(QString::fromStdString(gd.mainScene));
	if (!gd.previousLink.isEmpty())
		v.linkScene(QString::fromStdString(gd.mainScene), gd.previousLink);
	if (!g.previousScene.isEmpty())
		v.setCurrentScene(g.previousScene);
	for (const QString &s : {gd.sceneA, gd.sceneB})
		if (!s.isEmpty() && v.sceneNames().contains(s))
			v.removeScene(s);
	gd.sceneA.clear();
	gd.sceneB.clear();
	if (gd.createdMain) {
		OBSSourceAutoRelease src = obs_get_source_by_name(gd.mainScene.c_str());
		if (src)
			obs_source_remove(src);
		gd.createdMain = false;
	}
	gd.mainScene.clear();
	dockRestore();
}

/* Destination rows: tile from the platform or the server, settings button before
 * ▶ / ■ with clear tooltips, the settings button opens the destination dialog, and
 * a TikTok destination added from the dock menu shows the TikTok tile. */
ui::DestinationRow *dockRow(const std::string &id)
{
	ui::Dock *d = dockWidget();
	return d ? d->findChild<ui::DestinationRow *>(QString::fromStdString(id)) : nullptr;
}

void dockRowsSetup()
{
	core::Settings &st = V().settings();
	const struct {
		core::Platform p;
		const char *name;
		const char *server;
	} list[] = {{core::Platform::TikTok, "Selftest TikTok", ""},
		    /* saved as Instagram, but the server is TikTok's */
		    {core::Platform::Instagram, "Selftest TikTok server", "rtmp://push.tiktokcdn.com/game/"},
		    {core::Platform::Instagram, "Selftest Instagram", ""},
		    {core::Platform::CustomRtmp, "Selftest SRT", "srt://127.0.0.1:9999"}};
	for (const auto &e : list) {
		const std::string id = core::makeId((unsigned long long)QDateTime::currentMSecsSinceEpoch() +
							    gd.rowDestinations.size() + 100,
						    st.destinationIds());
		core::Destination d = core::makeDestination(e.p, id, e.name);
		d.server = e.server;
		st.destinations.push_back(d);
		gd.rowDestinations.push_back(id);
	}
	V().settingsEdited();
}

void dockCheckRows(const char *layout)
{
	const core::Platform want[] = {core::Platform::TikTok, core::Platform::TikTok, core::Platform::Instagram,
				       core::Platform::CustomSrt};
	int tiles = 0, buttons = 0;
	QStringList detail;
	for (size_t i = 0; i < gd.rowDestinations.size() && i < 4; i++) {
		ui::DestinationRow *row = dockRow(gd.rowDestinations[i]);
		if (!row) {
			detail << QStringLiteral("no row");
			continue;
		}
		const core::Platform tile = row->tile()->platform();
		if (tile == want[i])
			tiles++;
		QToolButton *gear = row->settingsButton();
		QToolButton *play = row->toggleButton();
		const core::Destination *d = V().settings().findDestination(gd.rowDestinations[i]);
		const QString name = d ? QString::fromStdString(d->name) : QString();
		const int gearRight = gear ? gear->geometry().right() : 0;
		const int playLeft = play ? play->geometry().left() : 0;
		const bool ok = gear && play && shown(gear) && shown(play) && gearRight < playLeft &&
				gear->size() == QSize(ui::tokens::kButton, ui::tokens::kButton) &&
				gear->size() == play->size() && gear->toolTip() == T("Dock.DestinationSettings") &&
				play->toolTip() == T("Dock.GoLive").arg(name);
		if (ok)
			buttons++;
		detail << QStringLiteral("%1: tile %2, gear %3..%4 play %5, tip '%6'")
				  .arg(name)
				  .arg(QString::fromUtf8(core::platformId(tile)))
				  .arg(gear ? gear->geometry().left() : -1)
				  .arg(gearRight)
				  .arg(playLeft)
				  .arg(play ? play->toolTip() : QString());
	}
	const QString text = detail.join(QStringLiteral(" | "));
	R->check(tiles == 4, layout[0] == 's' ? "dock-studio-row-tiles" : "dock-compact-row-tiles", text);
	R->check(buttons == 4,
		 layout[0] == 's' ? "dock-studio-row-settings-before-play" : "dock-compact-row-settings-before-play",
		 text);
}

/* Clicks the settings button of the row saved as Instagram with a TikTok server:
 * the destination dialog opens for it, with TikTok selected from the server. */
void dockRowSettingsOpensDialog()
{
	ui::DestinationRow *row = gd.rowDestinations.size() > 1 ? dockRow(gd.rowDestinations[1]) : nullptr;
	if (!row) {
		R->check(false, "dock-row-settings-opens-dialog", QStringLiteral("no row"));
		return;
	}
	auto seen = std::make_shared<QString>();
	QTimer::singleShot(400, [seen] {
		auto *dlg = qobject_cast<ui::DestinationDialog *>(QApplication::activeModalWidget());
		if (!dlg) {
			*seen = QStringLiteral("no dialog");
			return;
		}
		const core::Destination d = dlg->destination();
		*seen = QString::fromStdString(d.id) + QLatin1Char('|') +
			QString::fromUtf8(core::platformId(d.platform));
		dlg->reject();
	});
	row->settingsButton()->click(); /* modal: returns once the timer closed the dialog */
	const QString want = QString::fromStdString(gd.rowDestinations[1]) + QStringLiteral("|tiktok");
	R->check(*seen == want, "dock-row-settings-opens-dialog", *seen);
}

/* Each platform survives the dialog; a TikTok server switches the platform to TikTok. */
void dockDialogPlatforms()
{
	int ok = 0;
	for (core::Platform p : core::allPlatforms()) {
		ui::DestinationDialog dlg(core::makeDestination(p, "x", "x"));
		if (dlg.destination().platform == p)
			ok++;
	}
	R->check(ok == (int)core::allPlatforms().size(), "dock-dialog-keeps-platform",
		 QStringLiteral("%1/%2").arg(ok).arg(core::allPlatforms().size()));
	ui::DestinationDialog dlg(core::makeDestination(core::Platform::Instagram, "x", "x"));
	/* The server field is the line edit with the rtmps:// placeholder. */
	for (QLineEdit *e : dlg.findChildren<QLineEdit *>())
		if (e->placeholderText().startsWith(QStringLiteral("rtmps://")))
			e->setText(QStringLiteral("rtmp://push-rtmp-l11-va01.tiktokcdn.com/game/"));
	R->check(dlg.destination().platform == core::Platform::TikTok, "dock-dialog-platform-from-server",
		 QString::fromUtf8(core::platformId(dlg.destination().platform)));
}

/* The user's path: + Add destination > TikTok, OK in the dialog. */
void dockAddTikTokFromMenu()
{
	auto *add = dockChild<QToolButton>("addDestination");
	QAction *tiktok = nullptr;
	if (add && add->menu())
		for (QAction *a : add->menu()->actions())
			if (a->text() == QLatin1String("TikTok"))
				tiktok = a;
	if (!tiktok) {
		R->check(false, "dock-add-tiktok-from-menu", QStringLiteral("no TikTok entry"));
		return;
	}
	const std::vector<std::string> before = V().settings().destinationIds();
	auto seen = std::make_shared<QString>();
	QTimer::singleShot(400, [seen] {
		auto *dlg = qobject_cast<ui::DestinationDialog *>(QApplication::activeModalWidget());
		if (!dlg) {
			*seen = QStringLiteral("no dialog");
			return;
		}
		*seen = QString::fromUtf8(core::platformId(dlg->destination().platform));
		dlg->accept();
	});
	tiktok->trigger(); /* modal */
	std::string added;
	for (const std::string &id : V().settings().destinationIds())
		if (std::find(before.begin(), before.end(), id) == before.end())
			added = id;
	if (!added.empty())
		gd.rowDestinations.push_back(added);
	const core::Destination *d = added.empty() ? nullptr : V().settings().findDestination(added);
	ui::DestinationRow *row = added.empty() ? nullptr : dockRow(added);
	R->check(d && d->platform == core::Platform::TikTok && *seen == QLatin1String("tiktok") && row &&
			 row->tile()->platform() == core::Platform::TikTok,
		 "dock-add-tiktok-from-menu",
		 QStringLiteral("dialog %1, saved %2, tile %3")
			 .arg(*seen)
			 .arg(d ? QString::fromUtf8(core::platformId(d->platform)) : QStringLiteral("-"))
			 .arg(row ? QString::fromUtf8(core::platformId(row->tile()->platform()))
				  : QStringLiteral("-")));
}

void dockRowsRemove()
{
	auto &list = V().settings().destinations;
	list.erase(std::remove_if(list.begin(), list.end(),
				  [](const core::Destination &d) {
					  return std::find(gd.rowDestinations.begin(), gd.rowDestinations.end(),
							   d.id) != gd.rowDestinations.end();
				  }),
		   list.end());
	gd.rowDestinations.clear();
	V().settingsEdited();
}

/* Rows of the real streams (multistream group): live, time, Mbps. */
void dockCheckLiveRows()
{
	ui::Dock *d = dockWidget();
	int ok = 0;
	QStringList texts;
	const QString live = T("Status.Live");
	for (const std::string &id : g_multi) {
		auto *row = d ? d->findChild<ui::DestinationRow *>(QString::fromStdString(id)) : nullptr;
		const QString text = row ? row->statusText() : QStringLiteral("no row");
		texts << text;
		/* "Live · 00:00:03 · 6.0 Mbps" */
		const QStringList parts = text.split(QStringLiteral(" · "));
		const bool shape = parts.size() >= 3 && parts[0] == live && parts[1].size() == 8 &&
				   parts[1][2] == QLatin1Char(':') &&
				   parts[2].contains(QRegularExpression(QStringLiteral("^[0-9]+[.,][0-9] ")));
		auto *dot = row ? row->findChild<ui::StatusDot *>() : nullptr;
		if (row && shape && row->showsStop() && dot && dot->tone() == core::Tone::Live)
			ok++;
	}
	R->check(ok == (int)g_multi.size() && ok > 0, "dock-rows-live-status", texts.join(QStringLiteral(" | ")));
	auto *count = d ? d->findChild<QLabel *>(QStringLiteral("outputsCount")) : nullptr;
	const QString want = T("Dock.LiveCount").arg(g_multi.size());
	R->check(count && count->text() == want, "dock-outputs-live-count", count ? count->text() : QString());
	auto *frame = d ? d->findChild<ui::PreviewFrame *>(QStringLiteral("previewFrame")) : nullptr;
	R->check(frame && frame->live() && frame->preview()->overlayLive(), "dock-preview-live-frame");
}

void dockCheckRowsStopped()
{
	ui::Dock *d = dockWidget();
	int ok = 0;
	for (const std::string &id : g_multi) {
		auto *row = d ? d->findChild<ui::DestinationRow *>(QString::fromStdString(id)) : nullptr;
		if (row && !row->showsStop() && row->statusText().startsWith(T("Status.Stopped")))
			ok++;
	}
	R->check(ok == (int)g_multi.size(), "dock-rows-stopped", QStringLiteral("%1/%2").arg(ok).arg(g_multi.size()));
	auto *frame = d ? d->findChild<ui::PreviewFrame *>(QStringLiteral("previewFrame")) : nullptr;
	R->check(frame && !frame->live(), "dock-preview-frame-off");
}

/* Row of the unreachable destination (stream group): error or reconnecting. */
void dockCheckErrorRow()
{
	ui::Dock *d = dockWidget();
	auto *row = d ? d->findChild<ui::DestinationRow *>(QString::fromStdString(g.destinationId)) : nullptr;
	const OutputState st = O().destinationStatus(g.destinationId).state;
	auto *dot = row ? row->findChild<ui::StatusDot *>() : nullptr;
	const bool error = st == OutputState::Error;
	const QString prefix = error ? T("Status.Error") : T("Status.Reconnecting").section(QChar(0x2026), 0, 0);
	const bool ok = row && dot && dot->tone() == (error ? core::Tone::Error : core::Tone::Busy) &&
			row->statusText().startsWith(prefix) && row->showsStop() == !error;
	R->check(ok, "dock-row-error-status", row ? row->statusText() : QStringLiteral("no row"));
}

/* ------------------------------------------------------------------ screenshots */

/* The vertical canvas as an image (what the preview display shows). */
QImage canvasFrame()
{
	obs_canvas_t *canvas = V().canvas();
	obs_video_info ovi = {};
	if (!canvas || !obs_canvas_get_video_info(canvas, &ovi) || !ovi.base_width)
		return QImage();
	const uint32_t w = ovi.base_width / 2, h = ovi.base_height / 2;
	QImage out;
	obs_enter_graphics();
	gs_texrender_t *tr = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_BGRA);
	if (gs_texrender_begin(tr, w, h)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
		obs_render_canvas_texture(canvas);
		gs_texrender_end(tr);
		gs_stage_texture(stage, gs_texrender_get_texture(tr));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			out = QImage(data, (int)w, (int)h, (int)linesize, QImage::Format_ARGB32).copy();
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tr);
	obs_leave_graphics();
	return out;
}

void saveShot(const char *file)
{
	const char *dir = getenv("VERTICAL_SELFTEST_SHOTS");
	QDockWidget *frame = dockFrame();
	auto *pf = dockChild<ui::PreviewFrame>("previewFrame");
	if (!dir || !frame || !pf)
		return;
	QPixmap shot = frame->grab();
	QPainter p(&shot);
	ui::Preview *preview = pf->preview();
	if (preview->isVisible()) {
		const QPoint at = preview->mapTo(frame, QPoint(0, 0));
		p.drawImage(at, preview->snapshot(canvasFrame()));
	}
	p.end();
	QDir().mkpath(QString::fromUtf8(dir));
	const QString path = QDir(QString::fromUtf8(dir)).filePath(QString::fromUtf8(file));
	const bool ok = shot.save(path);
	R->check(ok, "dock-screenshot", QStringLiteral("%1 %2x%3").arg(path).arg(shot.width()).arg(shot.height()));
}

/* A scene that looks like a vertical show: gradient, a figure and a lower third. */
QString makeShotImage()
{
	QImage img(1080, 1920, QImage::Format_RGB32);
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing);
	QRadialGradient bg(QPointF(540, 576), 1300);
	bg.setColorAt(0.0, QColor(0x6d, 0x3f, 0xd0));
	bg.setColorAt(0.55, QColor(0x2a, 0x1a, 0x57));
	bg.setColorAt(1.0, QColor(0x12, 0x0c, 0x26));
	p.fillRect(img.rect(), bg);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xe8, 0xc9, 0xa8));
	p.drawEllipse(QPointF(540, 820), 140, 140);
	p.setBrush(QColor(0x1a, 0x1b, 0x22));
	p.drawRoundedRect(QRectF(240, 1000, 600, 500), 280, 280);
	QLinearGradient band(0, 1500, 0, 1920);
	band.setColorAt(0.0, QColor(91, 44, 200, 0));
	band.setColorAt(0.35, QColor(91, 44, 200, 217));
	p.fillRect(QRect(0, 1500, 1080, 420), band);
	QFont f;
	f.setPixelSize(110);
	f.setBold(true);
	f.setItalic(true);
	p.setFont(f);
	p.setPen(Qt::white);
	p.translate(540, 1720);
	p.rotate(-3);
	p.drawText(QRect(-540, -80, 1080, 160), Qt::AlignCenter, QStringLiteral("JUST CHATTING"));
	p.end();
	const QString path = QDir::temp().filePath(QStringLiteral("vertical-selftest-shot.png"));
	img.save(path);
	return path;
}

void shotsSetup()
{
	Vertical &v = V();
	if (g.previousScene.isEmpty())
		g.previousScene = v.currentScene();
	gd.shotImage = makeShotImage();
	for (const char *n : {"Talking", "Starting Soon", "Gameplay", "Be Right Back"})
		gd.shotScenes << v.addScene(QString::fromUtf8(n));
	OBSSourceAutoRelease sceneSrc = v.sceneSource(gd.shotScenes.front());
	if (obs_scene_t *scene = obs_scene_from_source(sceneSrc)) {
		OBSDataAutoRelease s = obs_data_create();
		obs_data_set_string(s, "file", gd.shotImage.toUtf8().constData());
		OBSSourceAutoRelease img = obs_source_create_private("image_source", "Selftest Show", s);
		obs_scene_add(scene, img);
	}
	v.setCurrentScene(gd.shotScenes.front());
	char **names = obs_frontend_get_scene_names();
	if (names && names[0]) {
		gd.mainScene = names[0];
		gd.previousLink = linkOf(names[0]);
		v.linkScene(QString::fromUtf8(names[0]), gd.shotScenes.front());
	}
	bfree(names);

	/* Two destinations live on local servers, two stopped. */
	const int base = atoi(getenv("VERTICAL_SELFTEST_RTMP_PORT")) + 3;
	core::Settings &st = v.settings();
	const struct {
		core::Platform p;
		const char *name;
		bool live;
	} list[] = {{core::Platform::TikTok, "TikTok LIVE", true},
		    {core::Platform::YouTube, "YouTube Live", true},
		    {core::Platform::Instagram, "Instagram Live", false},
		    {core::Platform::Facebook, "Facebook Live", false}};
	int port = base;
	for (const auto &e : list) {
		const std::string id = core::makeId((unsigned long long)QDateTime::currentMSecsSinceEpoch() +
							    gd.shotDestinations.size(),
						    st.destinationIds());
		core::Destination d = core::makeDestination(e.p, id, e.name);
		d.streamKey = "test";
		d.enabled = true;
		if (e.live)
			d.server = "rtmp://127.0.0.1:" + std::to_string(port++) + "/live";
		st.destinations.push_back(d);
		gd.shotDestinations.push_back(id);
	}
	v.settingsEdited();
	hotkeys::syncDestinations();
	for (size_t i = 0; i < 2 && i < gd.shotDestinations.size(); i++) {
		QString error;
		R->check(O().startDestination(gd.shotDestinations[i], &error), "shots-stream-start", error);
	}
	useTempRecordFolder();
	QString error;
	R->check(O().startRecording(&error), "shots-record-start", error);
	dockFloat(660, 800);
}

void shotsCleanup()
{
	O().stopRecording();
	for (const std::string &id : gd.shotDestinations)
		O().stopDestination(id);
}

void shotsRemove()
{
	Vertical &v = V();
	auto &list = v.settings().destinations;
	list.erase(std::remove_if(list.begin(), list.end(),
				  [](const core::Destination &d) {
					  return std::find(gd.shotDestinations.begin(), gd.shotDestinations.end(),
							   d.id) != gd.shotDestinations.end();
				  }),
		   list.end());
	gd.shotDestinations.clear();
	v.settingsEdited();
	hotkeys::syncDestinations();
	if (!gd.mainScene.empty()) {
		v.unlinkScene(QString::fromStdString(gd.mainScene));
		if (!gd.previousLink.isEmpty())
			v.linkScene(QString::fromStdString(gd.mainScene), gd.previousLink);
		gd.mainScene.clear();
	}
	if (!g.previousScene.isEmpty())
		v.setCurrentScene(g.previousScene);
	for (const QString &s : gd.shotScenes)
		v.removeScene(s);
	gd.shotScenes.clear();
	if (!gd.shotImage.isEmpty())
		QFile::remove(gd.shotImage);
	dockRestore();
}

/* ------------------------------------------------------------------ encoder warning */

void encoderWarningHidden(const char *name)
{
	auto *w = dockChild<QLabel>("footerEncoderWarning");
	R->check(w && !w->isVisible(), name,
		 QStringLiteral("hardware %1, software in use %2")
			 .arg(O().hardwareEncoderAvailable())
			 .arg(O().softwareEncoderInUse()));
}

void encoderForceX264()
{
	useTempRecordFolder();
	V().settings().recording.encoding.video.encoder = "obs_x264";
	V().settingsEdited();
	QString error;
	R->check(O().startRecording(&error), "dock-encoder-x264-record-start", error);
}

void encoderWarningShown()
{
	auto *w = dockChild<QLabel>("footerEncoderWarning");
	R->check(w && w->isVisible() && O().softwareEncoderInUse(), "dock-encoder-warning-shown-x264",
		 w ? w->text() + QStringLiteral(" | ") + w->toolTip() : QString());
	O().stopRecording();
}

void encoderRestore()
{
	V().settings().recording.encoding.video.encoder = g.savedRecording.encoding.video.encoder;
	V().settingsEdited();
}

/* ------------------------------------------------------------------ audio */

struct AudioState {
	QString wavPath;
	std::string voice; /* uuid: plays a 1 kHz tone */
	std::string music; /* uuid: silent, used for the exclusion bits */
	QString scene = QStringLiteral("Selftest Audio");
	QString previousProgram;
	QString collection;
	bool meterHeld = false;
	bool mainHooked = false;
	std::atomic<uint32_t> mainPeak{0};
	avsync::Config savedSync;
	int track = 0;
};
AudioState ga;

uint32_t peakBits(float f)
{
	uint32_t u;
	std::memcpy(&u, &f, sizeof(u));
	return u;
}

float bitsPeak(uint32_t u)
{
	float f;
	std::memcpy(&f, &u, sizeof(f));
	return f;
}

/* Peak of track 1 (the main mix), to check what stays in it. */
void mainMixCb(void *, size_t, struct audio_data *data)
{
	if (!data || !data->frames)
		return;
	const float p = core::peakOf(reinterpret_cast<const float *>(data->data[0]), data->frames);
	uint32_t cur = ga.mainPeak.load();
	while (peakBits(p) > cur && !ga.mainPeak.compare_exchange_weak(cur, peakBits(p))) {
	}
}

float takeMainPeak()
{
	return bitsPeak(ga.mainPeak.exchange(0));
}

float takeVerticalPeak()
{
	float l = 0.0f, r = 0.0f;
	AudioMix::instance().takePeaks(l, r);
	return std::max(l, r);
}

QString writeToneWav()
{
	const int rate = 48000, seconds = 2;
	const int frames = rate * seconds;
	QByteArray pcm;
	pcm.resize(frames * 4);
	auto *s = reinterpret_cast<int16_t *>(pcm.data());
	for (int i = 0; i < frames; i++) {
		const auto v = (int16_t)(0.5 * 32767.0 * std::sin(6.283185307179586 * 1000.0 * i / rate));
		s[2 * i] = s[2 * i + 1] = v;
	}
	QByteArray wav;
	const auto u32 = [&](uint32_t v) {
		wav.append(reinterpret_cast<const char *>(&v), 4);
	};
	const auto u16 = [&](uint16_t v) {
		wav.append(reinterpret_cast<const char *>(&v), 2);
	};
	wav.append("RIFF");
	u32(36 + (uint32_t)pcm.size());
	wav.append("WAVEfmt ");
	u32(16);
	u16(1);
	u16(2);
	u32(rate);
	u32(rate * 4);
	u16(4);
	u16(16);
	wav.append("data");
	u32((uint32_t)pcm.size());
	wav.append(pcm);
	const QString path = QDir::temp().filePath(QStringLiteral("vertical-selftest-tone.wav"));
	QFile f(path);
	if (f.open(QIODevice::WriteOnly))
		f.write(wav);
	return path;
}

OBSSourceAutoRelease byUuid(const std::string &uuid)
{
	return uuid.empty() ? nullptr : obs_get_source_by_uuid(uuid.c_str());
}

uint32_t mixersOf(const std::string &uuid)
{
	OBSSourceAutoRelease s = byUuid(uuid);
	return s ? obs_source_get_audio_mixers(s) : 0;
}

void audioSetup()
{
	ga.wavPath = writeToneWav();
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	ga.previousProgram = current ? QString::fromUtf8(obs_source_get_name(current)) : QString();
	obs_scene_t *scene = obs_scene_create(ga.scene.toUtf8().constData());
	OBSDataAutoRelease vs = obs_data_create();
	obs_data_set_bool(vs, "is_local_file", true);
	obs_data_set_string(vs, "local_file", ga.wavPath.toUtf8().constData());
	obs_data_set_bool(vs, "looping", true);
	obs_data_set_bool(vs, "restart_on_activate", true);
	OBSSourceAutoRelease voice = obs_source_create("ffmpeg_source", "Selftest Voice", vs, nullptr);
	OBSSourceAutoRelease music = obs_source_create("ffmpeg_source", "Selftest Music", nullptr, nullptr);
	obs_scene_add(scene, voice);
	obs_scene_add(scene, music);
	ga.voice = voice ? obs_source_get_uuid(voice) : "";
	ga.music = music ? obs_source_get_uuid(music) : "";
	obs_frontend_set_current_scene(obs_scene_get_source(scene));
	obs_scene_release(scene);
	AudioMix::instance().acquireMeter();
	ga.meterHeld = true;
	struct audio_convert_info conv = {};
	conv.samples_per_sec = 48000;
	conv.format = AUDIO_FORMAT_FLOAT_PLANAR;
	conv.speakers = SPEAKERS_STEREO;
	obs_add_raw_audio_callback(0, &conv, mainMixCb, nullptr);
	ga.mainHooked = true;
	ga.savedSync = avsync::config();
}

void audioTrackChecks()
{
	AudioMix &m = AudioMix::instance();
	ga.track = m.track();
	const uint32_t used = m.obsUsedTracks();
	R->check(ga.track >= 1 && ga.track <= 6 && !(used & core::trackBit(ga.track)), "audio-track-free",
		 QStringLiteral("track %1, OBS uses 0x%2").arg(ga.track).arg(used, 2, 16, QLatin1Char('0')));
	const auto rows = m.rows();
	const auto has = [&](const std::string &uuid) {
		return std::any_of(rows.begin(), rows.end(),
				   [&](const core::AudioRow &r) { return r.uuid == uuid && r.sent && r.onMainMix; });
	};
	R->check(has(ga.voice) && has(ga.music), "audio-sources-listed-and-sent",
		 QStringLiteral("%1 rows").arg(rows.size()));
	const uint32_t vb = core::trackBit(ga.track);
	R->check((mixersOf(ga.voice) & vb) && (mixersOf(ga.music) & vb) && (mixersOf(ga.voice) & 1),
		 "audio-new-sources-on-vertical-track",
		 QStringLiteral("voice 0x%1 music 0x%2").arg(mixersOf(ga.voice), 0, 16).arg(mixersOf(ga.music), 0, 16));
}

void audioBitChecks()
{
	AudioMix &m = AudioMix::instance();
	const uint32_t vb = core::trackBit(ga.track);
	m.setExcluded(ga.music, true);
	R->check(!(mixersOf(ga.music) & vb) && (mixersOf(ga.music) & 1), "audio-excluded-off-vertical-track",
		 QStringLiteral("0x%1").arg(mixersOf(ga.music), 0, 16));
	R->check(mixersOf(ga.voice) & vb, "audio-others-stay-on-vertical-track");
	/* The user changes the tracks of an excluded source: the vertical bit stays off,
	 * the other tracks are kept. */
	OBSSourceAutoRelease music = byUuid(ga.music);
	obs_source_set_audio_mixers(music, 0b000011u | vb);
	R->check(mixersOf(ga.music) == 0b000011u, "audio-user-change-keeps-rule",
		 QStringLiteral("0x%1").arg(mixersOf(ga.music), 0, 16));
	m.setExcluded(ga.music, false);
	R->check(mixersOf(ga.music) == (0b000011u | vb), "audio-included-back",
		 QStringLiteral("0x%1").arg(mixersOf(ga.music), 0, 16));
	/* Off track 1 (not in the main mix): off the vertical track too. */
	OBSSourceAutoRelease voice = byUuid(ga.voice);
	const uint32_t before = obs_source_get_audio_mixers(voice);
	obs_source_set_audio_mixers(voice, 0b000010u);
	R->check(mixersOf(ga.voice) == 0b000010u, "audio-off-main-off-vertical",
		 QStringLiteral("0x%1").arg(mixersOf(ga.voice), 0, 16));
	obs_source_set_audio_mixers(voice, 0b000001u);
	R->check(mixersOf(ga.voice) == (0b000001u | vb), "audio-on-main-on-vertical",
		 QStringLiteral("0x%1").arg(mixersOf(ga.voice), 0, 16));
	obs_source_set_audio_mixers(voice, before | vb);
}

void drainPeaks()
{
	takeVerticalPeak();
	takeMainPeak();
}

void audioLevelCheck(const char *name, bool wantVertical, int wantMain)
{
	const float v = takeVerticalPeak();
	const float mainPeak = takeMainPeak();
	bool ok = wantVertical ? v > 0.2f : v < 0.001f;
	if (wantMain >= 0)
		ok = ok && (wantMain ? mainPeak > 0.2f : mainPeak < 0.001f);
	R->check(ok, name, QStringLiteral("vertical peak %1, main peak %2").arg(v, 0, 'f', 4).arg(mainPeak, 0, 'f', 4));
}

void audioMute(bool on)
{
	OBSSourceAutoRelease voice = byUuid(ga.voice);
	obs_source_set_muted(voice, on);
	drainPeaks();
}

void audioExcludeVoice(bool on)
{
	AudioMix::instance().setExcluded(ga.voice, on);
	drainPeaks();
}

void audioMeterWidget()
{
	auto *meter = dockChild<ui::MixMeter>("verticalMixMeter");
	R->check(meter && meter->isVisible() && meter->receivedAudio() && meter->levelDb(0) > -20.0f &&
			 meter->levelDb(1) > -20.0f,
		 "audio-dock-meter-gets-levels",
		 meter ? QStringLiteral("L %1 dB, R %2 dB, height %3 px")
				 .arg(meter->levelDb(0), 0, 'f', 1)
				 .arg(meter->levelDb(1), 0, 'f', 1)
				 .arg(meter->height())
		       : QStringLiteral("no meter"));
	auto *preview = dockChild<QWidget>("previewFrame");
	R->check(meter && preview &&
			 meter->mapTo(dockWidget(), QPoint(0, 0)).y() >=
				 preview->mapTo(dockWidget(), QPoint(0, 0)).y() + preview->height() - 1,
		 "audio-dock-meter-under-preview");
}

void audioRecordStart()
{
	useTempRecordFolder();
	QString error;
	R->check(O().startRecording(&error), "audio-record-start", error);
}

void audioRecordCheck()
{
	const size_t mix = O().audioMixerFor(Outputs::recordId());
	R->check(mix == AudioMix::instance().mixIndex(), "audio-encoder-on-vertical-track",
		 QStringLiteral("mixer %1, vertical track %2").arg(mix + 1).arg(AudioMix::instance().track()));
	O().stopRecording();
}

uint64_t ga_flashBefore = 0;

void audioSyncTestStart()
{
	ga_flashBefore = synctest::flashFramesDrawn();
	const uint64_t t0 = synctest::trigger();
	R->check(t0 > os_gettime_ns(), "sync-test-scheduled",
		 QStringLiteral("in %1 ms").arg((int64_t)(t0 - os_gettime_ns()) / 1000000));
	drainPeaks();
}

void audioSyncTestCheck()
{
	const uint64_t frames = synctest::flashFramesDrawn() - ga_flashBefore;
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const int expected = (int)std::lround(synctest::kFlashMs / 1000.0 * ovi.fps_num / std::max(1u, ovi.fps_den));
	R->check((int)frames >= expected - 1 && (int)frames <= expected + 1, "sync-test-flash-frames",
		 QStringLiteral("%1 white frames (expected %2)").arg(frames).arg(expected));
	audioLevelCheck("sync-test-beep-on-vertical-only", true, 0);
	QFile f(QDir::temp().filePath(QStringLiteral("vertical-sync-test.json")));
	const bool ok = f.open(QIODevice::ReadOnly) &&
			f.readAll().contains(QByteArray::number((qulonglong)synctest::lastEventNs()));
	R->check(ok, "sync-test-event-file", f.fileName());
	OBSSourceAutoRelease ch1 = obs_canvas_get_channel(V().canvas(), 1);
	R->check(!!ch1, "sync-test-flash-on-channel-1-during-test");
	audioMute(false);
}

void audioSyncTestGone()
{
	OBSSourceAutoRelease ch1 = obs_canvas_get_channel(V().canvas(), 1);
	R->check(!ch1, "sync-test-cleans-up");
}

void audioOffsetPlan()
{
	avsync::setConfig({false, -50});
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const double fps = (double)ovi.fps_num / std::max(1u, ovi.fps_den);
	const core::SyncPlan p = core::planSync(-50, fps);
	R->check(avsync::videoDelayFrames() == p.videoDelayFrames &&
			 std::fabs(avsync::audioDelayMs() - p.audioDelayMs) < 0.01,
		 "sync-offset-negative-delays-image",
		 QStringLiteral("%1 frames + audio %2 ms").arg(avsync::videoDelayFrames()).arg(avsync::audioDelayMs()));
	avsync::setConfig({false, 40});
	R->check(avsync::videoDelayFrames() == 0 && std::fabs(avsync::audioDelayMs() - 40.0) < 0.01,
		 "sync-offset-positive-delays-audio");
	avsync::setConfig({true, 0});
	R->check(avsync::effectiveOffsetMs() == avsync::defaultOffsetMs(), "sync-offset-automatic-uses-default",
		 QStringLiteral("%1 ms").arg(avsync::defaultOffsetMs()));
}

/* Exclusions belong to the scene collection; the offset to the machine. */
void audioCollectionAway()
{
	AudioMix::instance().setExcluded(ga.music, true);
	obs_log(LOG_INFO, "[selftest] INFO music %s excluded %d", ga.music.c_str(),
		(int)V().settings().audio.exclusions.excluded(ga.music));
	avsync::setConfig({false, -37});
	char *name = obs_frontend_get_current_scene_collection();
	ga.collection = QString::fromUtf8(name ? name : "");
	bfree(name);
	if (ga.mainHooked) {
		obs_remove_raw_audio_callback(0, mainMixCb, nullptr);
		ga.mainHooked = false;
	}
	/* Made by the first run; later runs switch to it. */
	const char *other = "Selftest Audio Collection";
	if (!obs_frontend_add_scene_collection(other))
		obs_frontend_set_current_scene_collection(other);
	char *now = obs_frontend_get_current_scene_collection();
	R->check(now && std::strcmp(now, other) == 0, "audio-collection-switch-away",
		 QString::fromUtf8(now ? now : ""));
	bfree(now);
}

void audioCollectionOther()
{
	R->check(V().settings().audio.exclusions.size() == 0 && AudioMix::instance().track() >= 1,
		 "audio-other-collection-has-its-own-exclusions",
		 QStringLiteral("%1 exclusions, track %2")
			 .arg(V().settings().audio.exclusions.size())
			 .arg(AudioMix::instance().track()));
	obs_frontend_set_current_scene_collection(ga.collection.toUtf8().constData());
}

void audioCollectionBack()
{
	const uint32_t vb = core::trackBit(AudioMix::instance().track());
	R->check(V().settings().audio.exclusions.excluded(ga.music) && !(mixersOf(ga.music) & vb) &&
			 (mixersOf(ga.voice) & vb),
		 "audio-exclusions-survive-collection-switch",
		 QStringLiteral("music 0x%1 voice 0x%2").arg(mixersOf(ga.music), 0, 16).arg(mixersOf(ga.voice), 0, 16));
	const avsync::Config c = avsync::config();
	R->check(!c.automatic && c.offsetMs == -37, "audio-offset-survives-collection-switch",
		 QStringLiteral("%1 ms").arg(c.offsetMs));
}

void audioCleanup()
{
	if (ga.meterHeld) {
		AudioMix::instance().releaseMeter();
		ga.meterHeld = false;
	}
	if (ga.mainHooked) {
		obs_remove_raw_audio_callback(0, mainMixCb, nullptr);
		ga.mainHooked = false;
	}
	avsync::setConfig(ga.savedSync);
	AudioMix::instance().setExcluded(ga.music, false);
	AudioMix::instance().setExcluded(ga.voice, false);
	OBSSourceAutoRelease prev = obs_get_source_by_name(ga.previousProgram.toUtf8().constData());
	if (prev)
		obs_frontend_set_current_scene(prev);
	OBSSourceAutoRelease scene = obs_get_source_by_name(ga.scene.toUtf8().constData());
	for (const std::string &u : {ga.voice, ga.music}) {
		OBSSourceAutoRelease s = byUuid(u);
		if (s)
			obs_source_remove(s);
	}
	if (scene)
		obs_source_remove(scene);
	if (!ga.wavPath.isEmpty())
		QFile::remove(ga.wavPath);
}

/* ------------------------------------------------------------------ Vertical Audio driver */

void vaudioStart()
{
	const virtualaudio::Stats s = virtualaudio::stats();
	if (!s.installed) {
		R->check(!s.feeding, "vaudio-not-installed-idle",
			 QStringLiteral("driver not installed: feed checks skipped"));
		return;
	}
	virtualaudio::setForced(true);
}

void vaudioCheck()
{
	const virtualaudio::Stats s = virtualaudio::stats();
	if (!s.installed)
		return;
	R->check(s.feeding && s.chunks > 50 && s.underruns < 5, "vaudio-feeds-the-driver",
		 QStringLiteral("chunks %1, underruns %2, IO buffer %3 frames")
			 .arg(s.chunks)
			 .arg(s.underruns)
			 .arg(s.bufferFrames));
	obs_log(LOG_INFO, "[selftest] PERF vaudio schedule_ms=%.1f audio_delay_ms=%.1f path_ms=%.1f underruns=%llu",
		s.scheduleMs, s.audioDelayMs, s.scheduleMs + core::framesToMs(core::kLoopbackFrames),
		(unsigned long long)s.underruns);
	virtualaudio::setForced(false);
}

/* ------------------------------------------------------------------ measurement session */

/* Keeps OBS ready for vertical-sync-measure: remote sync tests on, the virtual camera
 * started when installed, the offset from VERTICAL_SELFTEST_OFFSET ("auto" or ms).
 * Ends when $TMPDIR/vertical-measure-done exists or after VERTICAL_SELFTEST_MEASURE_SECS. */
avsync::Config g_measureSaved;

QString measureDoneFile()
{
	return QDir::temp().filePath(QStringLiteral("vertical-measure-done"));
}

void measureStart()
{
	QFile::remove(measureDoneFile());
	g_measureSaved = avsync::config();
	const QString offset =
		QString::fromUtf8(getenv("VERTICAL_SELFTEST_OFFSET") ? getenv("VERTICAL_SELFTEST_OFFSET") : "auto");
	avsync::setConfig(offset == QLatin1String("auto") ? avsync::Config{true, 0}
							  : avsync::Config{false, offset.toInt()});
	synctest::setRemoteTrigger(true);
	QString error;
	const bool cam = O().startVirtualCam(&error);
	obs_log(LOG_INFO, "[selftest] INFO measure: offset %s, virtual camera %s %s, driver %s",
		offset.toUtf8().constData(), cam ? "started" : "not started", error.toUtf8().constData(),
		virtualaudio::stats().installed ? "installed" : "not installed");
}

void measureReport()
{
	const virtualaudio::Stats s = virtualaudio::stats();
	const core::LatencyInputs in = avsync::inputs();
	obs_log(LOG_INFO,
		"[selftest] PERF measure feeding=%d schedule_ms=%.1f underruns=%llu obs_audio_lag_ms=%.1f video_lag_ms=%.1f "
		"audio_path_ms=%.1f video_path_ms=%.1f default_offset_ms=%d effective_offset_ms=%d audio_delay_ms=%.1f "
		"video_delay_frames=%d",
		(int)s.feeding, s.scheduleMs, (unsigned long long)s.underruns, in.obsAudioLagMs, in.videoLagMs,
		core::audioPathMs(in), core::videoPathMs(in), avsync::defaultOffsetMs(), avsync::effectiveOffsetMs(),
		avsync::audioDelayMs(), avsync::videoDelayFrames());
}

void measureStop()
{
	measureReport();
	O().stopVirtualCam();
	synctest::setRemoteTrigger(false);
	avsync::setConfig(g_measureSaved);
	QFile::remove(measureDoneFile());
}

/* ------------------------------------------------------------------ 0.3 screenshots */

void saveWidgetShot(QWidget *w, const char *file)
{
	const char *dir = getenv("VERTICAL_SELFTEST_SHOTS");
	if (!dir || !w)
		return;
	QDir().mkpath(QString::fromUtf8(dir));
	const QString path = QDir(QString::fromUtf8(dir)).filePath(QString::fromUtf8(file));
	const bool ok = w->grab().save(path);
	R->check(ok, "screenshot", path);
}

QPointer<ui::SettingsDialog> g_settingsShot;

void settingsShot()
{
	g_settingsShot = new ui::SettingsDialog(static_cast<QWidget *>(obs_frontend_get_main_window()),
						ui::SettingsDialog::Tab::Audio);
	g_settingsShot->setAttribute(Qt::WA_DeleteOnClose);
	g_settingsShot->show();
}

void settingsShotSave()
{
	if (!g_settingsShot)
		return;
	auto *list = g_settingsShot->findChild<QWidget *>(QStringLiteral("verticalAudioSources"));
	auto *track = g_settingsShot->findChild<QLabel *>(QStringLiteral("verticalAudioTrack"));
	R->check(list && list->findChildren<QCheckBox *>().size() >= 2 && track &&
			 track->text().contains(QString::number(AudioMix::instance().track())),
		 "settings-audio-tab", track ? track->text() : QString());
	saveWidgetShot(g_settingsShot, "settings-audio.png");
	g_settingsShot->reject();
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
		dockCloseMenu();
		if (!gd.rowDestinations.empty())
			dockRowsRemove();
		dockRestore();
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
		/* A scene another plugin creates in the vertical canvas shows in the dock at once. */
		R->add(200, [] { obs_scene_release(obs_canvas_scene_create(V().canvas(), "Selftest External")); });
		R->waitUntil("scene-external-in-dock", 2000, [] {
			auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			QDockWidget *dock = main ? main->findChild<QDockWidget *>(QStringLiteral("vertical-dock"))
						 : nullptr;
			QListWidget *list = dock ? dock->findChild<QListWidget *>() : nullptr;
			return list &&
			       !list->findItems(QStringLiteral("Selftest External"), Qt::MatchExactly).isEmpty();
		});
		R->add(100, [] { V().removeScene(QStringLiteral("Selftest External")); });
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
		R->add(2500, [] { dockCheckLiveRows(); });
		R->add(2500, [] { multiCheck(); });
		R->waitUntil("multistream-stopped", 15000, [] { return !O().anyStreamActive(); });
		R->add(300, [] { dockCheckRowsStopped(); });
		R->add(200, [] { multiRemove(); });
		R->add(200, [] { dropRecordScene(); });
	}
	if (enabled("stream")) {
		R->add(300, [] { streamAdd(); });
		R->waitUntil("stream-reports-error", 8000, [] { return streamInError(); });
		R->add(300, [] { dockCheckErrorRow(); });
		R->add(0, [] { streamReport(); });
		R->waitUntil("stream-stopped", 10000, [] { return !O().anyStreamActive(); });
		R->add(200, [] { streamRemove(); });
	}
	if (enabled("dock")) {
		const auto modeIs = [](core::DockMode m) {
			return [m] {
				ui::Dock *d = dockWidget();
				return d && d->mode() == m;
			};
		};
		R->add(200, [] {
			R->check(ui::iconsAvailable(), "dock-icons-load");
			dockScenesSetup();
			dockFloat(700, 820);
		});
		R->waitUntil("dock-switches-to-studio", 3000, modeIs(core::DockMode::Studio));
		R->add(300, [] { dockCheckStudio(); });
		R->add(0, [] { dockFloat(515, 820); });
		R->add(600, [] {
			ui::Dock *d = dockWidget();
			R->check(d && d->mode() == core::DockMode::Studio, "dock-hysteresis-keeps-studio", widthText());
		});
		R->add(0, [] { dockFloat(320, 820); });
		R->waitUntil("dock-switches-to-compact", 3000, modeIs(core::DockMode::Compact));
		R->add(300, [] { dockCheckCompact(); });
		R->add(0, [] { dockWidget()->setLayoutChoice(core::LayoutChoice::Studio); });
		R->add(300, [] {
			ui::Dock *d = dockWidget();
			R->check(d && d->mode() == core::DockMode::Studio, "dock-forced-studio-when-narrow",
				 widthText());
			d->setLayoutChoice(core::LayoutChoice::Automatic);
		});
		R->add(300, [] {
			ui::Dock *d = dockWidget();
			R->check(d && d->mode() == core::DockMode::Compact, "dock-automatic-again", widthText());
			dockFloat(700, 820);
		});
		R->waitUntil("dock-back-to-studio", 3000, modeIs(core::DockMode::Studio));
		R->add(300, [] { dockClickScene(); });
		R->waitUntil("dock-scene-click-on-program", 2000, [] { return V().currentScene() == gd.sceneB; });
		R->add(100, [] { dockCheckSceneOnProgram(); });
		R->add(200, [] { dockOpenLinkMenu(); });
		R->waitUntil("dock-link-menu-open", 2000, [] { return openLinkMenu() != nullptr; });
		R->add(100, [] { dockLinkToggle(true); });
		R->add(200, [] { dockLinkToggle(false); });
		R->add(100, [] { dockCloseMenu(); });
		R->add(100, [] { dockLinkedSwitchSetup(); });
		R->waitUntil("dock-linked-switch-follows", 3000, [] { return V().currentScene() == gd.sceneB; });
		R->add(200, [] { dockLinkedSwitchUnlinked(); });
		R->add(600, [] { dockLinkedSwitchAgain(); });
		R->add(800, [] { dockLinkedSwitchCleanup(); });
		R->add(200, [] {
			useTempRecordFolder();
			dockRecordStart();
		});
		R->waitUntil("dock-record-live", 5000, [] { return O().recordingStatus().state == OutputState::Live; });
		R->add(2500, [] { dockRecordCheckRunning(); });
		R->waitUntil("dock-record-stopped", 15000, [] { return !O().recordingActive(); });
		R->add(200, [] { dockRecordCheckStopped(); });
		R->add(300, [] { encoderWarningHidden("dock-encoder-warning-hidden-hardware"); });
		R->add(0, [] { encoderForceX264(); });
		R->waitUntil("dock-encoder-x264-live", 5000,
			     [] { return O().recordingStatus().state == OutputState::Live; });
		R->add(1500, [] { encoderWarningShown(); });
		R->waitUntil("dock-encoder-x264-stopped", 15000, [] { return !O().recordingActive(); });
		R->add(300, [] {
			encoderRestore();
			encoderWarningHidden("dock-encoder-warning-hidden-after-stop");
		});
		R->add(200, [] { dockPreviewChecks(); });
		R->add(800, [] { dockPreviewWindowCheck(); });
		R->add(200, [] { dockRowsSetup(); });
		R->add(400, [] { dockCheckRows("studio"); });
		R->add(100, [] { dockRowSettingsOpensDialog(); });
		R->add(100, [] { dockDialogPlatforms(); });
		R->add(100, [] { dockAddTikTokFromMenu(); });
		R->add(0, [] { dockWidget()->setLayoutChoice(core::LayoutChoice::Compact); });
		R->add(400, [] {
			dockCheckRows("compact");
			dockWidget()->setLayoutChoice(core::LayoutChoice::Automatic);
		});
		R->add(200, [] { dockRowsRemove(); });
		R->add(300, [] { dockCleanup(); });
	}
	if (enabled("vaudio")) {
		R->add(200, [] { vaudioStart(); });
		R->add(3000, [] { vaudioCheck(); });
	}
	if (enabled("audio")) {
		R->add(200, [] { audioSetup(); });
		R->add(800, [] { audioTrackChecks(); });
		R->add(100, [] { audioBitChecks(); });
		R->add(200, [] { drainPeaks(); });
		R->add(1500, [] { audioLevelCheck("audio-vertical-mix-has-sound", true, 1); });
		R->add(0, [] { audioMute(true); });
		R->add(600, [] { drainPeaks(); });
		R->add(800, [] { audioLevelCheck("audio-muted-in-main-silent-in-vertical", false, 0); });
		R->add(0, [] { audioMute(false); });
		R->add(0, [] { audioExcludeVoice(true); });
		R->add(600, [] { drainPeaks(); });
		R->add(800, [] { audioLevelCheck("audio-excluded-silent-in-vertical-kept-in-main", false, 1); });
		R->add(0, [] { audioExcludeVoice(false); });
		R->add(0, [] { dockFloat(700, 820); });
		R->add(1500, [] { audioMeterWidget(); });
		R->add(0, [] { dockRestore(); });
		R->add(200, [] { audioRecordStart(); });
		R->waitUntil("audio-record-live", 5000,
			     [] { return O().recordingStatus().state == OutputState::Live; });
		R->add(500, [] { audioRecordCheck(); });
		R->waitUntil("audio-record-stopped", 15000, [] { return !O().recordingActive(); });
		R->add(0, [] { audioMute(true); }); /* only the beep on the tracks */
		R->add(1200, [] { audioSyncTestStart(); });
		R->add(synctest::kLeadMs + synctest::kFlashMs + 400, [] { audioSyncTestCheck(); });
		R->add(1500, [] { audioSyncTestGone(); });
		R->add(100, [] { audioOffsetPlan(); });
		R->add(200, [] { audioCollectionAway(); });
		R->add(4000, [] { audioCollectionOther(); });
		R->add(5000, [] { audioCollectionBack(); });
		R->add(200, [] { audioCleanup(); });
	}
	if (getenv("VERTICAL_SELFTEST_ONLY") && enabled("measure")) {
		const char *secs = getenv("VERTICAL_SELFTEST_MEASURE_SECS");
		R->add(500, [] { measureStart(); });
		R->waitUntil("measure-session", (secs ? atoi(secs) : 120) * 1000, [] {
			static uint64_t last = 0;
			if (os_gettime_ns() - last > 5000000000ull) {
				last = os_gettime_ns();
				measureReport();
			}
			return QFile::exists(measureDoneFile());
		});
		R->add(0, [] { measureStop(); });
	}
	if (getenv("VERTICAL_SELFTEST_SHOTS") && enabled("audioshots")) {
		R->add(200, [] { audioSetup(); });
		R->add(300, [] { dockFloat(700, 820); });
		R->add(1500, [] { saveShot("studio-meter.png"); });
		R->add(0, [] { dockFloat(320, 820); });
		R->waitUntil("audioshots-compact", 3000, [] {
			ui::Dock *d = dockWidget();
			return d && d->mode() == core::DockMode::Compact;
		});
		R->add(1500, [] { saveShot("compact-meter.png"); });
		R->add(0, [] { settingsShot(); });
		R->add(1200, [] { settingsShotSave(); });
		R->add(300, [] {
			dockRestore();
			audioCleanup();
		});
	}
	if (getenv("VERTICAL_SELFTEST_SHOTS") && getenv("VERTICAL_SELFTEST_RTMP_PORT")) {
		R->add(300, [] { shotsSetup(); });
		R->waitUntil("shots-live", 10000, [] {
			return gd.shotDestinations.size() >= 2 &&
			       O().destinationStatus(gd.shotDestinations[0]).state == OutputState::Live &&
			       O().destinationStatus(gd.shotDestinations[1]).state == OutputState::Live &&
			       O().recordingStatus().state == OutputState::Live;
		});
		R->add(4000, [] { saveShot("studio.png"); });
		R->add(0, [] { dockFloat(300, 820); });
		R->waitUntil("shots-compact", 3000, [] {
			ui::Dock *d = dockWidget();
			return d && d->mode() == core::DockMode::Compact;
		});
		R->add(300, [] { dockFloat(300, 820); });
		R->add(1200, [] { saveShot("compact.png"); });
		R->add(0, [] { shotsCleanup(); });
		R->waitUntil("shots-stopped", 15000, [] { return !O().anyStreamActive() && !O().recordingActive(); });
		R->add(200, [] { shotsRemove(); });
	}
	R->run();
}

} // namespace vertical
