#pragma once

/* Outputs of the vertical canvas: one obs_output per streaming destination,
 * the recording, the replay buffer and the virtual camera. Encoders come from
 * a pool keyed by their settings, so outputs with identical settings share
 * one video encoder and one audio encoder. UI thread only. */

#include "core/encoders.hpp"
#include "core/settings.hpp"

#include <obs.hpp>

#include <QObject>
#include <QString>
#include <QTimer>

#include <map>
#include <string>
#include <vector>

namespace vertical {

enum class OutputState { Idle, Starting, Live, Reconnecting, Stopping, Error };

struct OutputStatus {
	OutputState state = OutputState::Idle;
	QString error;
	uint64_t startedNs = 0;
	double kbps = -1.0; /* over the last second; < 0 until measured */
	int dropped = 0;
	int total = 0;
	int attempt = 0; /* reconnect attempt (1-based) while Reconnecting */
};

class Outputs : public QObject {
	Q_OBJECT

public:
	static Outputs &instance();

	/* Reconnection of the streams: attempts and delay between them. */
	static constexpr int kReconnectRetries = 25;
	static constexpr int kReconnectDelaySec = 2;

	/* ---- streaming ---- */
	bool startDestination(const std::string &id, QString *error);
	void stopDestination(const std::string &id);
	void toggleDestination(const std::string &id);
	void startAll();
	void stopAllStreams();
	OutputStatus destinationStatus(const std::string &id) const;
	bool anyStreamActive() const;

	/* ---- recording ---- */
	bool startRecording(QString *error);
	void stopRecording();
	OutputStatus recordingStatus() const { return status(kRecordId); }
	bool recordingActive() const;

	/* ---- replay buffer ---- */
	bool startReplay(QString *error);
	void stopReplay();
	bool saveReplay();
	bool replayActive() const;
	QString lastReplayPath() const { return lastReplay_; }

	/* ---- virtual camera ---- */
	bool startVirtualCam(QString *error);
	void stopVirtualCam();
	bool virtualCamActive() const;

	/* Stops everything now (collection switch, exit). */
	void stopEverything();
	int activeCount() const;

	/* "Apple VT H264 Hardware Encoder · 2 video encoders for 3 outputs". */
	QString encoderSummary() const;
	/* Number of destinations streaming (Live). */
	int liveStreamCount() const;
	/* Encoder types and output types found at runtime (logged once). */
	std::vector<core::EncoderCandidate> videoEncoders() const;

signals:
	void statusChanged();
	void replaySaved(const QString &path);
	void outputError(const QString &title, const QString &message);

private:
	static constexpr const char *kRecordId = "__record";
	static constexpr const char *kReplayId = "__replay";
	static constexpr const char *kVirtualCamId = "__vcam";

	struct PooledEncoder {
		std::string key;
		obs_encoder_t *encoder = nullptr; /* strong */
		int users = 0;
		std::string id;
		bool hardware = false;
	};

	struct Running {
		std::string id;
		obs_output_t *output = nullptr;   /* strong */
		obs_service_t *service = nullptr; /* strong, streams only */
		std::string videoKey;
		std::string audioKey;
		OutputStatus status;
		uint64_t lastBytes = 0;
		uint64_t lastBytesNs = 0;
		bool holdsRender = false;
	};

	Outputs();
	OutputStatus status(const std::string &id) const;
	Running *find(const std::string &id);
	const Running *find(const std::string &id) const;

	obs_encoder_t *acquireVideo(const core::EncoderSettings &s, QString *error);
	obs_encoder_t *acquireAudio(const core::EncoderSettings &s, QString *error);
	void releaseEncoders(Running &r);

	bool launch(Running r, const core::EncoderSettings &enc, bool needsAudio, QString *error);
	void connectSignals(obs_output_t *output);
	void disconnectSignals(obs_output_t *output);
	void onStarted(obs_output_t *output);
	void onStopped(obs_output_t *output, int code, const QString &lastError);
	void onReconnect(obs_output_t *output, bool reconnecting);
	void onReplaySaved();
	void finish(const std::string &id);
	void tick();
	QString recordFolder() const;

	static void startedCb(void *data, calldata_t *cd);
	static void stoppedCb(void *data, calldata_t *cd);
	static void reconnectCb(void *data, calldata_t *cd);
	static void reconnectOkCb(void *data, calldata_t *cd);
	static void replaySavedCb(void *data, calldata_t *cd);

	std::vector<PooledEncoder> video_;
	std::vector<PooledEncoder> audio_;
	std::map<std::string, Running> running_;
	std::map<std::string, OutputStatus> lastStatus_; /* errors stay visible after stop */
	QString lastReplay_;
	QTimer stats_;
};

} // namespace vertical
