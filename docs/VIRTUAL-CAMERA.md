# Vertical Camera (macOS): a 9:16 virtual camera

Goal: TikTok LIVE Studio (and any app that reads a camera) sees a camera named
**Vertical Camera** that shows the vertical canvas of the plugin in 9:16, while OBS's own
virtual camera stays free for the main 16:9 program.

Since 0.3 the camera ships with the **Vertical Audio** microphone (the vertical mix) in one
package, "Vertical Camera & Audio": see [VIRTUAL-AUDIO.md](VIRTUAL-AUDIO.md) for the driver,
the vertical mix and the audio/video offset of the pair.

Status: design + scaffold. The extension, its host app and the plugin-side sender compile
(see "What was checked"). Nothing is signed, installed or tested on a live system yet:
that needs the Apple portal steps in section 6.

## 1. Why not OBS's virtual camera

* OBS has **one** virtual camera, and it is tied to the main output: it shows the main
  program (or a single source, scene or the preview), at the main canvas size. It cannot
  show another canvas.
* Its camera extension (`com.obsproject.obs-studio.mac-camera-extension`), its device
  UUID and the Mach service name are OBS's. They are signed by the OBS team and embedded
  in OBS.app. A plugin cannot add a second device to that extension, and writing into its
  sink would fight with OBS's own output.
* So the plugin ships its **own** Camera Extension, with its own device, signed by us.
  Both cameras can run at the same time.

## 2. Architecture

```
OBS process                                       Camera extension process (sandboxed)
-----------------------------------------         ---------------------------------------
vertical canvas (obs_canvas, 1080x1920)           company.theonair.vertical-camera.extension
  video_t ── obs_output "vertical_virtualcam"       device "Vertical Camera"
             raw NV12 frames at the camera size       stream 0: source  ──► camera clients
             memcpy into a pooled IOSurface-backed     stream 1: sink    ◄── plugin
             CVPixelBuffer, CMSampleBuffer           sink buffers are forwarded to the source;
             CMSimpleQueueEnqueue(sink queue) ──────► placeholder frame when nothing arrives
                         (CoreMediaIO moves the IOSurface, no copy)
```

### 2.1 Camera extension (Swift, `camera-extension/extension/`)

A CoreMediaIO Camera Extension (`CMIOExtensionProvider`, macOS 13+), packaged as a
system extension (`.systemextension`, product type `com.apple.product-type.system-extension`).

* `ProviderSource.swift`: the provider, one device.
* `DeviceSource.swift`: the device "Vertical Camera", model "Vertical Camera", transport
  type virtual. It adds the **source** stream first and the **sink** stream second (the
  plugin relies on this order).
* Formats (both streams): 1080x1920 and 720x1280, 4:2:0 bi-planar video range (`420v`),
  30 or 60 fps. `420v` is the native output of the OBS canvas (NV12), so neither side
  converts on the CPU. If the client picks a format that differs from what the plugin
  writes, the extension scales with `VTPixelTransferSession` (GPU).
* `StreamSink.swift`: the sink, queue size 2, 1 buffer required for startup. While the
  sink runs, a timer pulls buffers (`consumeSampleBuffer(from:)`) twice per frame at
  60 fps, forwards them to the source (`stream.send`) and acknowledges them
  (`notifyScheduledOutputChanged`).
* `StreamSource.swift`: what clients read. Active format and frame duration are settable
  by the client.
* **Placeholder**: when no frame came from the plugin for 500 ms (or the sink is not
  running), the source sends a dark frame "Vertical Camera / Start the virtual camera in
  the Vertical dock of OBS", drawn once per format. No image file is bundled.
* UUIDs (fixed, in `camera-extension/CMakeLists.txt` and the extension Info.plist):

  | What | UUID |
  | --- | --- |
  | Device (also the CMIO device UID the plugin looks for) | `D0378109-93F1-4BA1-A12A-5C7B7B9BC58E` |
  | Source stream | `E4C44F98-E06C-4F3B-8969-3EA833FB7C8C` |
  | Sink stream | `1EDE1E2A-B0A8-4816-BE2D-67793D1904D2` |

  The device UUID is also `kDeviceUID` in `src/obs/virtualcam.hpp`. Never change them
  after a release: apps remember cameras by UID.

### 2.2 Host app (Swift, `camera-extension/app/`)

macOS only activates a system extension at the request of the **app bundle that embeds
it** (`Contents/Library/SystemExtensions/<bundle id>.systemextension`), and that app must
be in `/Applications` (otherwise `OSSystemExtensionErrorUnsupportedParentBundleLocation`).
A `.plugin` loaded by OBS is not an app: it cannot activate an extension, and OBS.app is
not ours to modify. So we ship a tiny app, **Vertical Camera.app**:

* a window with **Install** and **Uninstall** buttons
  (`OSSystemExtensionRequest.activationRequest` / `deactivationRequest`), a status line
  (approval needed, done, needs reboot, wrong location);
* command-line flags `--install` / `--uninstall`, used by the plugin
  (`virtualcam::openHostApp(true)`, "Install the camera" in the dock) and possibly by the
  `.pkg` post-install script;
* on version change it always replaces the installed extension.

### 2.3 Plugin side (`src/obs/virtualcam.hpp`, `src/obs/virtualcam-mac.mm`)

* Output type `vertical_virtualcam` (`OBS_OUTPUT_VIDEO`, raw video), registered by
  `vertical::virtualcam::registerOutput()`.
* The owner of the canvas creates it and binds it to the canvas:
  `obs_output_set_media(out, obs_canvas_get_video(canvas), obs_get_audio())`.
  It acquires the render gate **VirtualCam** reason before `obs_output_start`, and
  releases it on the output's `stop` signal. The camera therefore costs nothing when off.
* `start`: asks libobs for NV12, video range, at the camera size
  (`obs_output_set_video_conversion`; settings `width`/`height`, default 1080x1920).
  libobs does that on the GPU; if the canvas output already is NV12 at that size, frames
  pass through. Then it finds the device among `kCMIOHardwarePropertyDevices` by
  `kCMIODevicePropertyDeviceUID`, takes the **second** stream of
  `kCMIODevicePropertyStreams` (the sink), `CMIOStreamCopyBufferQueue`,
  `CMIODeviceStartStream`.
* `raw_video`: one `memcpy` per plane into a pooled IOSurface-backed `CVPixelBuffer`,
  YCbCr matrix / primaries / transfer attachments (709, or 601 if the canvas uses 601),
  `CMSampleBufferCreateForImageBuffer` with the OBS timestamp (host clock, ns),
  `CMSimpleQueueEnqueue`. When the queue is full the oldest frame is dropped (live, never
  late); frames not handed over at stop are released.
* Audio/video offset (0.3): a negative offset delays the image by whole frames, held
  before `CMSimpleQueueEnqueue` (the pooled IOSurface buffers themselves, no copy; at
  most 500 ms); the frame lag is measured on every frame for the default offset
  (VIRTUAL-AUDIO.md, section 3).
* Errors (`obs_output_get_last_error`): `VirtualCam.Error.NotInstalled` (device not
  found: extension not installed or not approved), `VirtualCam.Error.NotStarted`
  (device found but the sink cannot start), `VirtualCam.Error.NoCanvas`.
* `isExtensionInstalled()` (device visible), `isHostAppPresent()`,
  `openHostApp(install)` drive the dock UI: "Install the camera" when missing, "Start
  camera" otherwise.
* Built only with the CMake option `VERTICAL_VIRTUALCAM=ON` (macOS). Frameworks:
  CoreMediaIO, CoreMedia, CoreVideo, Foundation, AppKit (IOSurface is used through
  CoreVideo's `kCVPixelBufferIOSurfacePropertiesKey`).

Why the CMIO sink and not our own XPC/shared memory: the sink queue is Apple's supported
cross-process path for camera extensions. Buffers are IOSurfaces, so only a handle crosses
the process boundary. A sandboxed camera extension cannot easily host a custom Mach
service for an unsandboxed OBS process, and a private shared-memory channel would need
its own synchronisation and lifetime handling for no gain. OBS's own virtual camera uses
the same sink mechanism.

## 3. Identifiers

| Item | Value |
| --- | --- |
| Team | `562LS75A65` (Rise (Apps)) |
| Plugin bundle | `company.theonair.obs-vertical` |
| Host app | `company.theonair.vertical-camera` ("Vertical Camera.app") |
| Extension | `company.theonair.vertical-camera.extension` (the bundle file must be named `company.theonair.vertical-camera.extension.systemextension`) |
| App group | `562LS75A65.company.theonair.vertical-camera` |
| Mach service (`CMIOExtensionMachServiceName`) | `562LS75A65.company.theonair.vertical-camera.extension` |

`CMIOExtensionMachServiceName` (Info.plist of the extension, key `CMIOExtension`) is the
Mach service through which the system camera service talks to the extension. A
sandboxed extension may only register a Mach service whose name **starts with one of its
application groups**, hence the team-prefixed group in the extension entitlements and the
service name built as `<group>.extension`. Team-prefixed groups (`TEAMID.something`) do
not need to be registered in the portal on macOS.

## 4. Files

```
camera-extension/
  CMakeLists.txt                  targets vertical-camera-extension, vertical-camera-app (Xcode generator only)
  extension/main.swift            reads the UUIDs from Info.plist, starts the provider
  extension/ProviderSource.swift
  extension/DeviceSource.swift    formats, placeholder, sink -> source forwarding, GPU scaling
  extension/StreamSource.swift
  extension/StreamSink.swift
  app/main.swift, app/App.swift   host app: Install / Uninstall
  audio-driver/VerticalAudio.cpp  Vertical Audio driver (AudioServerPlugIn), VIRTUAL-AUDIO.md
  tools/sync-measure/main.swift   vertical-sync-measure (audio/video offset, latencies)
  cmake/driver-Info.plist.in      CFPlugIn factory of the driver
  cmake/extension-Info.plist.in   CMIOExtension dict, UUIDs
  cmake/app-Info.plist.in
  cmake/extension.entitlements.in sandbox + app group
  cmake/app.entitlements.in       com.apple.developer.system-extension.install + app group
  scripts/sign-and-notarize.sh    inside-out re-sign (+ driver), notarize, staple
  scripts/make-pkg.sh             "Vertical Camera & Audio" package
src/obs/virtualcam.hpp, src/obs/virtualcam-mac.mm
```

## 5. Building

```sh
# compile check, no signing (ad hoc, no entitlements: cannot be activated)
cmake -S camera-extension -B build_camera -G Xcode
cmake --build build_camera --config Release
# -> build_camera/Release/Vertical Camera.app
#      Contents/Library/SystemExtensions/company.theonair.vertical-camera.extension.systemextension

# or from the plugin: cmake --preset macos -DVERTICAL_BUILD_CAMERA=ON -DVERTICAL_VIRTUALCAM=ON
```

Signed build, once section 6 is done:

```sh
cmake -S camera-extension -B build_camera -G Xcode \
  -DCODESIGN_IDENTITY="Developer ID Application: Rise (Apps) (562LS75A65)" \
  -DVERTICAL_CAMERA_PROFILE_HOST="Vertical Camera Developer ID" \
  -DVERTICAL_CAMERA_PROFILE_EXT="Vertical Camera Extension Developer ID"
cmake --build build_camera --config Release
camera-extension/scripts/sign-and-notarize.sh "build_camera/Release/Vertical Camera.app" \
  ~/Downloads/Vertical_Camera_Developer_ID.provisionprofile \
  ~/Downloads/Vertical_Camera_Extension_Developer_ID.provisionprofile
```

How the build uses the signing inputs:

* `CODESIGN_IDENTITY`: the Developer ID Application identity. Empty = ad hoc signature
  without entitlements (compile check only).
* `VERTICAL_CAMERA_PROFILE_HOST` / `VERTICAL_CAMERA_PROFILE_EXT`: either the **name or
  UUID** of an installed profile (Xcode resolves it, `PROVISIONING_PROFILE_SPECIFIER`) or
  a **path** to a `.provisionprofile` (copied to `Contents/embedded.provisionprofile`
  before Xcode signs). The profile must be embedded: it is what grants
  `com.apple.developer.system-extension.install` to a Developer ID app.
* Entitlements come from `cmake/*.entitlements.in` (team id from `VERTICAL_TEAM_ID`).
  Hardened runtime and secure timestamp are on.
* Order: Xcode builds and signs the extension, the app's post-build step copies it into
  `Contents/Library/SystemExtensions/`, then Xcode signs the app, sealing the embedded
  extension. `scripts/sign-and-notarize.sh` redoes this by hand, inside-out, with
  `codesign --force --timestamp --options runtime --entitlements ...` (extension first,
  then app, never `--deep` for signing), then
  `xcrun notarytool submit <zip> --keychain-profile rise-notary --wait` and
  `xcrun stapler staple "Vertical Camera.app"`.
* Packaging: the plugin `.pkg` installs for the current user only, and macOS activates a
  camera extension only from `/Applications`, so the camera ships as its own package,
  `obs-vertical-camera-<version>-macos-universal.pkg`, displayed as "Vertical Camera &
  Audio" since 0.3. `scripts/make-pkg.sh` builds it from the signed and notarized app and
  the signed audio driver: the app installs exactly into `/Applications`, the driver into
  `/Library/Audio/Plug-Ins/HAL` (neither relocatable; administrator password), a
  postinstall script restarts coreaudiod; the product is signed with **Developer ID
  Installer: Rise (Apps)**, then notarized and stapled.

## 6. Apple Developer setup (done)

Everything below exists on the Rise (Apps) team (562LS75A65) and on the release Mac:

1. **App IDs** `company.theonair.vertical-camera` (capability **System Extension**) and
   `company.theonair.vertical-camera.extension`.
2. **Developer ID profiles** `Vertical Camera Developer ID` (host app, contains
   `com.apple.developer.system-extension.install`) and
   `Vertical Camera Extension Developer ID`, installed in
   `~/Library/Developer/Xcode/UserData/Provisioning Profiles/`.
3. **Certificates** "Developer ID Application: Rise (Apps)" (code) and "Developer ID
   Installer: Rise (Apps)" (packages), in the login keychain.
4. **Notarization credentials**: keychain profile `rise-notary`, made once with
   ```sh
   xcrun notarytool store-credentials rise-notary \
     --apple-id <Apple ID email> --team-id 562LS75A65 --password <app-specific password>
   ```

Signed build of the camera:
```sh
cmake -S camera-extension -B build_camera -G Xcode \
  -DCODESIGN_IDENTITY="Developer ID Application: Rise (Apps) (562LS75A65)" \
  -DVERTICAL_CAMERA_PROFILE_HOST="Vertical Camera Developer ID" \
  -DVERTICAL_CAMERA_PROFILE_EXT="Vertical Camera Extension Developer ID" \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build_camera --config Release -- ONLY_ACTIVE_ARCH=NO
camera-extension/scripts/sign-and-notarize.sh "build_camera/Release/Vertical Camera.app" \
  <host .provisionprofile> <extension .provisionprofile> build_camera/Release/VerticalAudio.driver
camera-extension/scripts/make-pkg.sh "build_camera/Release/Vertical Camera.app" \
  build_camera/Release/VerticalAudio.driver obs-vertical-camera-0.3.0-macos-universal.pkg
signing/brand-pkg.sh obs-vertical-camera-0.3.0-macos-universal.pkg vertical-camera <branded .pkg>
```

## 7. Testing and development

* Prefer **signed Developer ID builds** even in development. The alternative,
  `systemextensionsctl developer on`, needs SIP partially disabled (Recovery mode); it
  lets an extension load from outside `/Applications` but is not worth it on the
  production Mac.
* Install: copy `Vertical Camera.app` to `/Applications`, open it, click Install, approve
  in System Settings > General > Login Items & Extensions > **Camera Extensions**
  (macOS 15+; on 13/14: Privacy & Security, "System software from ... was blocked").
* Check: `systemextensionsctl list` shows `company.theonair.vertical-camera.extension`
  `[activated enabled]`. Photo Booth or QuickTime > New Movie Recording should list
  "Vertical Camera" with the placeholder.
* Logs: `log stream --predicate 'subsystem == "company.theonair.vertical-camera.extension"'`
  and the OBS log lines `[obs-vertical] virtual camera started (1080x1920)`.
* Apps cache the camera list: restart TikTok LIVE Studio / browsers after installing.
* **Uninstall**: Vertical Camera.app > Uninstall (or `open -a "Vertical Camera" --args
  --uninstall`), approve, then delete the app. Removing the app alone also schedules
  the extension's removal. Leftovers: `systemextensionsctl list`; a stuck one goes away
  with `systemextensionsctl uninstall 562LS75A65 company.theonair.vertical-camera.extension`
  (needs SIP off) or after a reboot once the app is gone.

## 8. TikTok LIVE Studio notes

* It picks cameras by **name**: the device is named "Vertical Camera", distinct from
  "OBS Virtual Camera". Keep the name stable.
* Use a 9:16 scene/layout in LIVE Studio and select the 1080x1920 format; the camera
  offers only portrait formats, so there is no letterboxing to undo.
* Audio does not go through a camera: pick the **Vertical Audio** microphone (0.3), which
  carries the vertical mix aligned with this camera (VIRTUAL-AUDIO.md).
* Expected latency: one or two frames (sink queue of 2, live drop policy), plus the image
  delay of a negative audio/video offset.

## 9. Limits and open questions

* Activation and the image in a client app (Photo Booth, TikTok LIVE Studio) have not been
  checked on a real Mac yet: the app is signed, notarized and accepted by Gatekeeper.
* Which writers may feed the sink: today any local process may connect to the sink
  (`authorizedToStartStream` returns true). Restrict to signed OBS
  (`CMIOExtensionClient` gives pid / signing id)?
* Formats: 1080x1920 and 720x1280 at 30/60 fps. Is 1440x2560 wanted? Should the camera
  follow the canvas size exactly (the extension would need re-activation to change its
  format list, so fixed formats are simpler)?
* Should the `.pkg` run `Vertical Camera.app --install` automatically after install
  (one approval prompt at install time), or leave it to the dock button?
* One product name for both: the camera shows as "Vertical Camera"; the app is
  "Vertical Camera.app". Fine, or prefer "Vertical for OBS Camera"?
* Intel Macs: the packaged build is universal (arm64 and x86_64), not tried on an Intel Mac.
* macOS 13/14 vs 15+: approval UI differs (section 7); no code difference.

## 10. What was checked

* `swiftc -typecheck` (macOS 13 target, arm64 and x86_64) on the extension and on the
  host app: clean.
* `cmake -S camera-extension -B build_camera -G Xcode` and an unsigned Release build:
  both targets build; the app contains
  `Contents/Library/SystemExtensions/company.theonair.vertical-camera.extension.systemextension`
  whose Info.plist has the Mach service and UUIDs above.
* `src/obs/virtualcam-mac.mm`: `clang++ -fsyntax-only -Wall -Wextra` against the OBS 33
  headers, with and without ARC, arm64 and x86_64: clean.
* Signed universal Release build with both Developer ID profiles: `codesign --verify
  --strict --deep` clean, notarized (Accepted) and stapled; `spctl` says "Notarized
  Developer ID". The camera package is notarized and accepted by `spctl --type install`,
  and its payload is `./Vertical Camera.app` installed into `/Applications`.
* Not done yet: activation on a real Mac and a frame actually reaching a client app.
