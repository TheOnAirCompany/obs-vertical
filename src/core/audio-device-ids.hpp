#pragma once

/* Identifiers of the Vertical Audio driver (camera-extension/audio-driver), shared by
 * the driver, the OBS plugin and the tests. Never change them after a release: apps
 * remember audio devices by UID. Macros so that the driver can use them in CFSTR(). */

/* "Vertical Audio": the microphone apps see (input only, visible). */
#define VERTICAL_AUDIO_INPUT_UID "company.theonair.vertical-audio.input"
/* "Vertical Audio Feed": what OBS plays into (output only, hidden). */
#define VERTICAL_AUDIO_FEED_UID "company.theonair.vertical-audio.feed"
#define VERTICAL_AUDIO_MODEL_UID "company.theonair.vertical-audio.model"
#define VERTICAL_AUDIO_DEVICE_NAME "Vertical Audio"
#define VERTICAL_AUDIO_DRIVER_PATH "/Library/Audio/Plug-Ins/HAL/VerticalAudio.driver"
