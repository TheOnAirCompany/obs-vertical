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

#pragma once

/* 9:16 virtual camera (macOS): an obs_output that writes the frames of the vertical
 * canvas into the sink stream of the "Vertical Camera" CoreMediaIO extension
 * (camera-extension/). See docs/VIRTUAL-CAMERA.md.
 *
 * Usage (owner of the vertical canvas):
 *   obs_data_t *s = obs_data_create();
 *   obs_data_set_int(s, "width", 1080);  obs_data_set_int(s, "height", 1920);
 *   obs_output_t *out = obs_output_create(virtualcam::kOutputId, "Vertical Camera", s, nullptr);
 *   obs_output_set_media(out, obs_canvas_get_video(canvas), obs_get_audio());
 *   // acquire the render gate "VirtualCam" reason, then:
 *   obs_output_start(out);   // fails with obs_output_get_last_error() if not installed
 * The output emits the usual "start" / "stop" signals; release the render gate reason
 * on "stop". */

namespace vertical::virtualcam {

/* Output type id registered by registerOutput(). */
inline constexpr const char *kOutputId = "vertical_virtualcam";

/* Must match VerticalCameraDeviceUUID in camera-extension/cmake/extension-Info.plist.in
 * (the CMIO device UID of an extension device is its device UUID string). */
inline constexpr const char *kDeviceUID = "D0378109-93F1-4BA1-A12A-5C7B7B9BC58E";

/* The host app that embeds and activates the extension. */
inline constexpr const char *kHostAppBundleId = "company.theonair.vertical-camera";
inline constexpr const char *kHostAppPath = "/Applications/Vertical Camera.app";

/* Default camera size; the extension also offers 720x1280 (it scales on the GPU). */
inline constexpr int kDefaultWidth = 1080;
inline constexpr int kDefaultHeight = 1920;

#ifdef __APPLE__
/* Registers the "vertical_virtualcam" output type. Call once from obs_module_load. */
void registerOutput();
/* True when the "Vertical Camera" device is visible to CoreMediaIO (extension active). */
bool isExtensionInstalled();
/* True when the host app is present in /Applications. */
bool isHostAppPresent();
/* Opens the host app; with install = true it asks it to activate the extension
 * (macOS then shows its approval prompt). Returns false if the app is missing. */
bool openHostApp(bool install);
#else
inline void registerOutput() {}
inline bool isExtensionInstalled()
{
	return false;
}
inline bool isHostAppPresent()
{
	return false;
}
inline bool openHostApp(bool)
{
	return false;
}
#endif

} // namespace vertical::virtualcam
