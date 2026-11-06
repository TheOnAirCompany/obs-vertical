#include "obs/outputs.hpp"

#include "core/dock-model.hpp"
#include "obs/audio-mix.hpp"
#include "obs/av-sync.hpp"
#include "obs/text.hpp"
#include "obs/vertical.hpp"
#ifdef VERTICAL_VIRTUALCAM
#include "obs/virtualcam.hpp"
#endif

#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QMetaObject>

#include <algorithm>

namespace vertical {

namespace {

std::vector<std::string> outputTypes()
{
	std::vector<std::string> out;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_output_types(i, &id); i++)
		out.emplace_back(id);
	return out;
}

std::vector<std::string> audioEncoderIds()
{
	std::vector<std::string> out;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++)
		if (obs_get_encoder_type(id) == OBS_ENCODER_AUDIO)
			out.emplace_back(id);
	return out;
}

bool contains(const std::string &s, const char *part)
{
	return s.find(part) != std::string::npos;
}

OBSDataAutoRelease videoSettings(const std::string &encoderId, const std::string &codec, const core::VideoEncoding &v)
{
	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_int(s, "bitrate", v.bitrateKbps);
	obs_data_set_int(s, "keyint_sec", v.keyintSec);
	obs_data_set_string(s, "rate_control", v.rateControl.c_str());
	std::string profile = v.profile;
	if (codec == "hevc" && profile == "high")
		profile = "main";
	obs_data_set_string(s, "profile", profile.c_str());
	if (contains(encoderId, "videotoolbox")) {
		obs_data_set_bool(s, "bframes", v.bframes);
	} else if (encoderId == "obs_x264") {
		obs_data_set_string(s, "preset", "veryfast");
		if (!v.bframes)
			obs_data_set_string(s, "x264opts", "bframes=0");
	} else {
		obs_data_set_int(s, "bf", v.bframes ? 2 : 0);
	}
	return s;
}

QString issueText(const std::vector<core::Issue> &issues)
{
	QStringList parts;
	for (const core::Issue &i : issues)
		if (i.severity == core::Severity::Error)
			parts << T(core::issueKey(i.code));
	return parts.join(QStringLiteral("\n"));
}

} // namespace

Outputs &Outputs::instance()
{
	static Outputs *o = new Outputs();
	return *o;
}

Outputs::Outputs()
{
	stats_.setInterval(1000);
	connect(&stats_, &QTimer::timeout, this, &Outputs::tick);
}

std::vector<core::EncoderCandidate> Outputs::videoEncoders() const
{
	std::vector<core::EncoderCandidate> out;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		const char *codec = obs_get_encoder_codec(id);
		const char *name = obs_encoder_get_display_name(id);
		out.push_back({id, codec ? codec : "", name ? name : ""});
	}
	return out;
}

/* =================================================================== encoder pool */

obs_encoder_t *Outputs::acquireVideo(const core::EncoderSettings &s, QString *error)
{
	const std::string key = s.videoKey();
	for (PooledEncoder &p : video_)
		if (p.key == key) {
			p.users++;
			return p.encoder;
		}

	const core::EncoderChoice choice = core::chooseVideoEncoder(videoEncoders(), s.video);
	if (choice.id.empty()) {
		if (error)
			*error = T("Error.NoVideoEncoder");
		return nullptr;
	}
	if (choice.requestedMissing)
		obs_log(LOG_WARNING, "encoder '%s' is not available, using '%s'", s.video.encoder.c_str(),
			choice.id.c_str());
	if (!choice.hardware)
		obs_log(LOG_WARNING, "no hardware video encoder found, using '%s' (CPU)", choice.id.c_str());

	video_t *video = obs_canvas_get_video(Vertical::instance().canvas());
	if (!video) {
		if (error)
			*error = T("Error.NoCanvasVideo");
		return nullptr;
	}
	OBSDataAutoRelease settings = videoSettings(choice.id, choice.codec, s.video);
	const std::string name = "Vertical video " + std::to_string(video_.size() + 1);
	obs_encoder_t *enc = obs_video_encoder_create(choice.id.c_str(), name.c_str(), settings, nullptr);
	if (!enc) {
		if (error)
			*error = T("Error.EncoderCreate");
		return nullptr;
	}
	obs_encoder_set_video(enc, video);
	obs_log(LOG_INFO, "video encoder '%s' (%s, %d kbps) created", choice.id.c_str(),
		choice.hardware ? "hardware" : "software", s.video.bitrateKbps);
	video_.push_back({key, enc, 1, choice.id, choice.hardware});
	return enc;
}

obs_encoder_t *Outputs::acquireAudio(const core::EncoderSettings &s, QString *error)
{
	const std::string key = s.audioKey();
	for (PooledEncoder &p : audio_)
		if (p.key == key) {
			p.users++;
			return p.encoder;
		}
	const std::string id = core::chooseAudioEncoder(audioEncoderIds());
	if (id.empty()) {
		if (error)
			*error = T("Error.NoAudioEncoder");
		return nullptr;
	}
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", s.audio.bitrateKbps);
	const std::string name = "Vertical audio " + std::to_string(audio_.size() + 1);
	/* Track 0: the vertical mix, with the user's exclusions (the track Vertical manages). */
	const size_t mixer = s.audio.track == 0 ? AudioMix::instance().mixIndex()
						: (size_t)std::clamp(s.audio.track, 1, core::kTrackCount) - 1;
	obs_encoder_t *enc = obs_audio_encoder_create(id.c_str(), name.c_str(), settings, mixer, nullptr);
	if (!enc) {
		if (error)
			*error = T("Error.EncoderCreate");
		return nullptr;
	}
	obs_encoder_set_audio(enc, obs_get_audio());
	obs_log(LOG_INFO, "audio encoder '%s' (%d kbps) on track %zu", id.c_str(), s.audio.bitrateKbps, mixer + 1);
	audio_.push_back({key, enc, 1, id, false});
	return enc;
}

void Outputs::releaseEncoders(Running &r)
{
	const auto drop = [](std::vector<PooledEncoder> &pool, const std::string &key) {
		for (auto it = pool.begin(); it != pool.end(); ++it)
			if (it->key == key) {
				if (--it->users <= 0) {
					obs_encoder_release(it->encoder);
					pool.erase(it);
				}
				return;
			}
	};
	if (!r.videoKey.empty())
		drop(video_, r.videoKey);
	if (!r.audioKey.empty())
		drop(audio_, r.audioKey);
	r.videoKey.clear();
	r.audioKey.clear();
}

/* =================================================================== launch / finish */

bool Outputs::launch(Running r, const core::EncoderSettings &enc, bool needsAudio, QString *error)
{
	Vertical &v = Vertical::instance();
	if (!v.ensureCanvas() || (!obs_canvas_get_video(v.canvas()) && !v.resetVideo())) {
		if (error)
			*error = T("Error.NoCanvasVideo");
		obs_output_release(r.output);
		obs_service_release(r.service);
		return false;
	}

	obs_encoder_t *venc = acquireVideo(enc, error);
	if (!venc) {
		obs_output_release(r.output);
		obs_service_release(r.service);
		return false;
	}
	r.videoKey = enc.videoKey();
	if (needsAudio) {
		obs_encoder_t *aenc = acquireAudio(enc, error);
		if (!aenc) {
			releaseEncoders(r);
			obs_output_release(r.output);
			obs_service_release(r.service);
			return false;
		}
		r.audioKey = enc.audioKey();
		obs_output_set_audio_encoder(r.output, aenc, 0);
	}
	obs_output_set_video_encoder(r.output, venc);

	connectSignals(r.output);
	v.acquireRender(core::RenderReason::Output);
	r.holdsRender = true;
	r.status = OutputStatus();
	r.status.state = OutputState::Starting;
	const std::string id = r.id;
	obs_output_t *output = r.output;
	running_[id] = r;
	lastStatus_.erase(id);

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		const QString msg = err && *err ? QString::fromUtf8(err) : T("Error.StartFailed");
		if (error)
			*error = msg;
		finish(id);
		OutputStatus failed;
		failed.state = OutputState::Error;
		failed.error = msg;
		lastStatus_[id] = failed;
		emit statusChanged();
		return false;
	}
	if (!stats_.isActive())
		stats_.start();
	emit statusChanged();
	return true;
}

void Outputs::finish(const std::string &id)
{
	auto it = running_.find(id);
	if (it == running_.end())
		return;
	Running r = it->second;
	running_.erase(it);
	disconnectSignals(r.output);
	obs_output_release(r.output);
	obs_service_release(r.service);
	releaseEncoders(r);
	if (r.holdsRender)
		Vertical::instance().releaseRender(id == kVirtualCamId ? core::RenderReason::VirtualCam
								       : core::RenderReason::Output);
	if (running_.empty())
		stats_.stop();
	emit statusChanged();
}

Outputs::Running *Outputs::find(const std::string &id)
{
	auto it = running_.find(id);
	return it == running_.end() ? nullptr : &it->second;
}

const Outputs::Running *Outputs::find(const std::string &id) const
{
	auto it = running_.find(id);
	return it == running_.end() ? nullptr : &it->second;
}

OutputStatus Outputs::status(const std::string &id) const
{
	if (const Running *r = find(id))
		return r->status;
	auto it = lastStatus_.find(id);
	return it == lastStatus_.end() ? OutputStatus() : it->second;
}

/* =================================================================== streaming */

bool Outputs::startDestination(const std::string &id, QString *error)
{
	if (find(id))
		return true;
	const core::Destination *d = Vertical::instance().settings().findDestination(id);
	if (!d) {
		if (error)
			*error = T("Error.UnknownDestination");
		return false;
	}
	const auto issues = core::validate(*d);
	if (core::hasErrors(issues)) {
		if (error)
			*error = issueText(issues);
		return false;
	}
	const core::StreamUrl url = core::parseStreamUrl(d->server);

	OBSDataAutoRelease ss = obs_data_create();
	obs_data_set_string(ss, "server", d->server.c_str());
	obs_data_set_string(ss, "key", d->streamKey.c_str());
	obs_data_set_bool(ss, "use_auth", false);
	obs_data_set_bool(ss, "bwtest", false);
	const std::string label = "Vertical " + d->name;

	Running r;
	r.id = id;
	r.service = obs_service_create_private("rtmp_custom", label.c_str(), ss);
	r.output = obs_output_create(core::outputTypeFor(url.protocol), label.c_str(), nullptr, nullptr);
	if (!r.service || !r.output) {
		obs_service_release(r.service);
		obs_output_release(r.output);
		if (error)
			*error = T("Error.StartFailed");
		return false;
	}
	obs_output_set_service(r.output, r.service);
	obs_output_set_reconnect_settings(r.output, kReconnectRetries, kReconnectDelaySec);
	obs_log(LOG_INFO, "starting '%s' (%s, %s)", d->name.c_str(), core::protocolName(url.protocol),
		url.host.c_str()); /* never log the key */
	return launch(std::move(r), d->encoding, true, error);
}

void Outputs::stopDestination(const std::string &id)
{
	Running *r = find(id);
	if (!r)
		return;
	r->status.state = OutputState::Stopping;
	obs_output_stop(r->output);
	emit statusChanged();
}

void Outputs::toggleDestination(const std::string &id)
{
	if (find(id)) {
		stopDestination(id);
		return;
	}
	QString error;
	if (!startDestination(id, &error)) {
		const core::Destination *d = Vertical::instance().settings().findDestination(id);
		emit outputError(d ? QString::fromStdString(d->name) : QString(), error);
	}
}

void Outputs::startAll()
{
	for (const core::Destination &d : Vertical::instance().settings().destinations) {
		if (!d.enabled || find(d.id))
			continue;
		QString error;
		if (!startDestination(d.id, &error))
			emit outputError(QString::fromStdString(d.name), error);
	}
}

void Outputs::stopAllStreams()
{
	std::vector<std::string> ids;
	for (const auto &[id, r] : running_)
		if (id != kRecordId && id != kReplayId && id != kVirtualCamId)
			ids.push_back(id);
	for (const std::string &id : ids)
		stopDestination(id);
}

OutputStatus Outputs::destinationStatus(const std::string &id) const
{
	return status(id);
}

bool Outputs::anyStreamActive() const
{
	return std::any_of(running_.begin(), running_.end(), [](const auto &p) {
		return p.first != kRecordId && p.first != kReplayId && p.first != kVirtualCamId;
	});
}

/* =================================================================== recording */

QString Outputs::recordFolder() const
{
	const std::string &path = Vertical::instance().settings().recording.path;
	if (!path.empty())
		return QString::fromStdString(path);
	char *main = obs_frontend_get_current_record_output_path();
	const QString folder = QString::fromUtf8(main ? main : "");
	bfree(main);
	return folder;
}

bool Outputs::startRecording(QString *error)
{
	if (find(kRecordId))
		return true;
	const core::RecordingSettings &rs = Vertical::instance().settings().recording;
	const core::MuxerChoice m = core::chooseMuxer(rs.format, outputTypes());
	if (m.fellBack)
		obs_log(LOG_WARNING, "hybrid %s is not available, recording to %s", core::recordFormatId(rs.format),
			m.extension.c_str());
	const QString folder = recordFolder();
	if (folder.isEmpty() || !os_file_exists(folder.toUtf8().constData())) {
		if (error)
			*error = T("Error.RecordFolder");
		return false;
	}
	char *file = os_generate_formatted_filename(m.extension.c_str(), true, rs.filenameFormat.c_str());
	const QString full = folder + QStringLiteral("/") + QString::fromUtf8(file);
	bfree(file);

	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_string(s, "path", full.toUtf8().constData());
	Running r;
	r.id = kRecordId;
	r.output = obs_output_create(m.outputType.c_str(), "Vertical Recording", s, nullptr);
	if (!r.output) {
		if (error)
			*error = T("Error.StartFailed");
		return false;
	}
	obs_log(LOG_INFO, "recording to %s (%s)", full.toUtf8().constData(), m.outputType.c_str());
	return launch(std::move(r), rs.encoding, true, error);
}

void Outputs::stopRecording()
{
	stopDestination(kRecordId);
}

bool Outputs::recordingActive() const
{
	return find(kRecordId) != nullptr;
}

/* =================================================================== replay buffer */

bool Outputs::startReplay(QString *error)
{
	if (find(kReplayId))
		return true;
	const core::Settings &st = Vertical::instance().settings();
	const QString folder = recordFolder();
	if (folder.isEmpty()) {
		if (error)
			*error = T("Error.RecordFolder");
		return false;
	}
	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_string(s, "directory", folder.toUtf8().constData());
	const std::string format = "Vertical Replay %CCYY-%MM-%DD %hh-%mm-%ss";
	obs_data_set_string(s, "format", format.c_str());
	obs_data_set_string(s, "extension", core::replayExtension(st.recording.format).c_str());
	obs_data_set_bool(s, "allow_spaces", true);
	obs_data_set_int(s, "max_time_sec", st.replay.seconds);
	obs_data_set_int(s, "max_size_mb", st.replay.maxMb);
	Running r;
	r.id = kReplayId;
	r.output = obs_output_create("replay_buffer", "Vertical Replay Buffer", s, nullptr);
	if (!r.output) {
		if (error)
			*error = T("Error.StartFailed");
		return false;
	}
	signal_handler_connect(obs_output_get_signal_handler(r.output), "saved", replaySavedCb, this);
	return launch(std::move(r), st.recording.encoding, true, error);
}

void Outputs::stopReplay()
{
	stopDestination(kReplayId);
}

bool Outputs::saveReplay()
{
	Running *r = find(kReplayId);
	if (!r || r->status.state != OutputState::Live)
		return false;
	calldata_t cd = {};
	proc_handler_call(obs_output_get_proc_handler(r->output), "save", &cd);
	calldata_free(&cd);
	return true;
}

bool Outputs::replayActive() const
{
	return find(kReplayId) != nullptr;
}

void Outputs::onReplaySaved()
{
	Running *r = find(kReplayId);
	if (!r)
		return;
	calldata_t cd = {};
	proc_handler_call(obs_output_get_proc_handler(r->output), "get_last_replay", &cd);
	lastReplay_ = QString::fromUtf8(calldata_string(&cd, "path"));
	calldata_free(&cd);
	emit replaySaved(lastReplay_);
}

/* =================================================================== virtual camera */

bool Outputs::startVirtualCam(QString *error)
{
#ifdef VERTICAL_VIRTUALCAM
	if (find(kVirtualCamId))
		return true;
	if (!virtualcam::isExtensionInstalled()) {
		if (error)
			*error = T("VirtualCam.Error.NotInstalled");
		return false;
	}
	Vertical &v = Vertical::instance();
	if (!v.ensureCanvas() || (!obs_canvas_get_video(v.canvas()) && !v.resetVideo())) {
		if (error)
			*error = T("VirtualCam.Error.NoCanvas");
		return false;
	}
	OBSDataAutoRelease s = obs_data_create();
	obs_data_set_int(s, "width", virtualcam::kDefaultWidth);
	obs_data_set_int(s, "height", virtualcam::kDefaultHeight);
	Running r;
	r.id = kVirtualCamId;
	r.output = obs_output_create(virtualcam::kOutputId, "Vertical Camera", s, nullptr);
	if (!r.output) {
		if (error)
			*error = T("VirtualCam.Error.NotStarted");
		return false;
	}
	obs_output_set_media(r.output, obs_canvas_get_video(v.canvas()), obs_get_audio());
	connectSignals(r.output);
	v.acquireRender(core::RenderReason::VirtualCam);
	r.holdsRender = true;
	r.status.state = OutputState::Starting;
	obs_output_t *output = r.output;
	running_[kVirtualCamId] = r;
	if (!obs_output_start(output)) {
		if (error)
			*error = T("VirtualCam.Error.NotStarted");
		finish(kVirtualCamId);
		return false;
	}
	emit statusChanged();
	return true;
#else
	if (error)
		*error = T("VirtualCam.Error.NotInstalled");
	return false;
#endif
}

void Outputs::stopVirtualCam()
{
	stopDestination(kVirtualCamId);
}

bool Outputs::virtualCamActive() const
{
	return find(kVirtualCamId) != nullptr;
}

/* =================================================================== global */

void Outputs::stopEverything()
{
	std::vector<std::string> ids;
	for (const auto &[id, r] : running_)
		ids.push_back(id);
	for (const std::string &id : ids) {
		Running *r = find(id);
		if (!r)
			continue;
		disconnectSignals(r->output);
		obs_output_force_stop(r->output);
		finish(id);
	}
	lastStatus_.clear();
}

int Outputs::activeCount() const
{
	return (int)running_.size();
}

QString Outputs::encoderSummary() const
{
	if (video_.empty()) {
		const core::EncoderChoice c = core::chooseVideoEncoder(videoEncoders(), core::VideoEncoding());
		if (c.id.empty())
			return T("Encoder.None");
		const char *name = obs_encoder_get_display_name(c.id.c_str());
		return T(c.hardware ? "Encoder.WillUseHardware" : "Encoder.WillUseSoftware")
			.arg(QString::fromUtf8(name ? name : c.id.c_str()));
	}
	int users = 0;
	for (const PooledEncoder &p : video_)
		users += p.users;
	const char *name = obs_encoder_get_display_name(video_.front().id.c_str());
	return T("Encoder.Active")
		.arg(QString::fromUtf8(name ? name : video_.front().id.c_str()))
		.arg(video_.size())
		.arg(users);
}

int Outputs::liveStreamCount() const
{
	int n = 0;
	for (const auto &[id, r] : running_)
		if (id != kRecordId && id != kReplayId && id != kVirtualCamId && r.status.state == OutputState::Live)
			n++;
	return n;
}

bool Outputs::softwareEncoderInUse() const
{
	if (!video_.empty())
		return std::any_of(video_.begin(), video_.end(), [](const PooledEncoder &p) { return !p.hardware; });
	return false;
}

bool Outputs::hardwareEncoderAvailable() const
{
	return core::chooseVideoEncoder(videoEncoders(), core::VideoEncoding()).hardware;
}

size_t Outputs::audioMixerFor(const std::string &id) const
{
	const Running *r = find(id);
	obs_encoder_t *enc = r ? obs_output_get_audio_encoder(r->output, 0) : nullptr;
	return enc ? obs_encoder_get_mixer_index(enc) : SIZE_MAX;
}

void Outputs::tick()
{
	avsync::update();
	const uint64_t now = os_gettime_ns();
	for (auto &[id, r] : running_) {
		if (r.status.state != OutputState::Live && r.status.state != OutputState::Reconnecting)
			continue;
		const uint64_t bytes = obs_output_get_total_bytes(r.output);
		const double kbps = core::kbpsBetween(r.lastBytes, r.lastBytesNs, bytes, now);
		if (kbps >= 0.0)
			r.status.kbps = kbps;
		r.lastBytes = bytes;
		r.lastBytesNs = now;
		r.status.dropped = obs_output_get_frames_dropped(r.output);
		r.status.total = obs_output_get_total_frames(r.output);
	}
	emit statusChanged();
}

/* =================================================================== signals */

void Outputs::connectSignals(obs_output_t *output)
{
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_connect(sh, "start", startedCb, this);
	signal_handler_connect(sh, "stop", stoppedCb, this);
	signal_handler_connect(sh, "reconnect", reconnectCb, this);
	signal_handler_connect(sh, "reconnect_success", reconnectOkCb, this);
}

void Outputs::disconnectSignals(obs_output_t *output)
{
	if (!output)
		return;
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_disconnect(sh, "start", startedCb, this);
	signal_handler_disconnect(sh, "stop", stoppedCb, this);
	signal_handler_disconnect(sh, "reconnect", reconnectCb, this);
	signal_handler_disconnect(sh, "reconnect_success", reconnectOkCb, this);
	signal_handler_disconnect(sh, "saved", replaySavedCb, this);
}

void Outputs::onStarted(obs_output_t *output)
{
	for (auto &[id, r] : running_)
		if (r.output == output) {
			r.status.state = OutputState::Live;
			r.status.startedNs = os_gettime_ns();
			r.status.error.clear();
		}
	emit statusChanged();
}

void Outputs::onStopped(obs_output_t *output, int code, const QString &lastError)
{
	std::string id;
	for (const auto &[k, r] : running_)
		if (r.output == output)
			id = k;
	if (id.empty())
		return;
	OutputStatus st;
	if (code != OBS_OUTPUT_SUCCESS) {
		st.state = OutputState::Error;
		st.error = lastError.isEmpty() ? T("Error.Disconnected") : lastError;
		obs_log(LOG_WARNING, "output '%s' stopped with code %d", id.c_str(), code);
	}
	finish(id);
	lastStatus_[id] = st;
	emit statusChanged();
}

void Outputs::onReconnect(obs_output_t *output, bool reconnecting)
{
	for (auto &[id, r] : running_)
		if (r.output == output) {
			r.status.state = reconnecting ? OutputState::Reconnecting : OutputState::Live;
			/* "reconnect" fires once per attempt. */
			r.status.attempt = reconnecting ? r.status.attempt + 1 : 0;
			/* The byte counter starts again with the new connection. */
			r.lastBytesNs = 0;
		}
	emit statusChanged();
}

void Outputs::startedCb(void *data, calldata_t *cd)
{
	auto *self = static_cast<Outputs *>(data);
	auto *output = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	QMetaObject::invokeMethod(self, [self, output] { self->onStarted(output); }, Qt::QueuedConnection);
}

void Outputs::stoppedCb(void *data, calldata_t *cd)
{
	auto *self = static_cast<Outputs *>(data);
	auto *output = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	const int code = (int)calldata_int(cd, "code");
	const char *err = obs_output_get_last_error(output);
	const QString msg = QString::fromUtf8(err ? err : "");
	QMetaObject::invokeMethod(
		self, [self, output, code, msg] { self->onStopped(output, code, msg); }, Qt::QueuedConnection);
}

void Outputs::reconnectCb(void *data, calldata_t *cd)
{
	auto *self = static_cast<Outputs *>(data);
	auto *output = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	QMetaObject::invokeMethod(self, [self, output] { self->onReconnect(output, true); }, Qt::QueuedConnection);
}

void Outputs::reconnectOkCb(void *data, calldata_t *cd)
{
	auto *self = static_cast<Outputs *>(data);
	auto *output = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	QMetaObject::invokeMethod(self, [self, output] { self->onReconnect(output, false); }, Qt::QueuedConnection);
}

void Outputs::replaySavedCb(void *data, calldata_t *)
{
	auto *self = static_cast<Outputs *>(data);
	QMetaObject::invokeMethod(self, [self] { self->onReplaySaved(); }, Qt::QueuedConnection);
}

} // namespace vertical
