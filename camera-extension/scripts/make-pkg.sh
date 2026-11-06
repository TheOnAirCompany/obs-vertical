#!/bin/zsh
# Packages "Vertical Camera & Audio": the signed and notarized "Vertical Camera.app" into
# /Applications (macOS activates camera extensions only from there) and the signed
# VerticalAudio.driver into /Library/Audio/Plug-Ins/HAL (where coreaudiod loads audio
# drivers). Neither component is relocatable. A postinstall script restarts coreaudiod
# so that "Vertical Audio" appears at once (the Mac's sound stops for about a second);
# on an update over an activated camera, another one has Vertical Camera.app replace
# the extension.
# The product is signed with the Developer ID Installer identity, notarized and stapled.
#
#   camera-extension/scripts/make-pkg.sh "build_camera/Release/Vertical Camera.app" \
#     build_camera/Release/VerticalAudio.driver obs-vertical-camera-0.3.0-macos-universal.pkg
#
# Environment:
#   INSTALLER_IDENT  default: Developer ID Installer: Rise (Apps) (562LS75A65)
#   NOTARY_PROFILE   notarytool keychain profile (default: rise-notary)
set -euo pipefail

app=${1:?path to Vertical Camera.app}; app=${app:a}
driver=${2:?path to VerticalAudio.driver}; driver=${driver:a}
out=${3:?output .pkg}; out=${out:a}
ident=${INSTALLER_IDENT:-"Developer ID Installer: Rise (Apps) (562LS75A65)"}
notary=${NOTARY_PROFILE:-rise-notary}
app_id=$(defaults read "$app/Contents/Info" CFBundleIdentifier)
driver_id=$(defaults read "$driver/Contents/Info" CFBundleIdentifier)
version=$(defaults read "$app/Contents/Info" CFBundleShortVersionString)

spctl --assess --type execute "$app" || { print -u2 "sign and notarize the app first (sign-and-notarize.sh)"; exit 1 }
sig=$(codesign -dvv "$driver" 2>&1)
[[ $sig == *"Authority=Developer ID Application"* && $sig == *"(runtime)"* && $sig == *$'\nTimestamp='* ]] ||
  { print -u2 "the driver must be signed with Developer ID, hardened runtime and a timestamp"; exit 1 }

work=$(mktemp -d)
trap 'rm -rf $work' EXIT

# Installs exactly where given: never relocated onto another copy found elsewhere.
component() { # <root> <plist out>
  pkgbuild --analyze --root $1 $2 >/dev/null
  plutil -replace 0.BundleIsRelocatable -bool NO $2
  plutil -replace 0.BundleOverwriteAction -string upgrade $2
}

# 1. The camera app. Over an activated camera (an update), the app replaces the extension
#    itself (old build out, new build in), so the camera does not vanish until a reboot.
mkdir -p $work/camera $work/camera-scripts
ditto "$app" "$work/camera/${app:t}"
component $work/camera $work/camera.plist
cat > $work/camera-scripts/postinstall <<'SH'
#!/bin/sh
# Update over an activated Vertical Camera: let Vertical Camera.app swap the extension.
if /usr/bin/systemextensionsctl list 2>/dev/null | grep -q "company.theonair.vertical-camera.extension.*activated enabled"; then
  user=$(stat -f %Su /dev/console)
  if [ -n "$user" ] && [ "$user" != root ]; then
    launchctl asuser "$(id -u "$user")" sudo -u "$user" \
      open -n -a "/Applications/Vertical Camera.app" --args --install --quit-when-done || true
  fi
fi
exit 0
SH
chmod 755 $work/camera-scripts/postinstall
pkgbuild --root $work/camera --component-plist $work/camera.plist --scripts $work/camera-scripts \
  --identifier "$app_id.pkg" --version "$version" --install-location /Applications \
  $work/camera.pkg >/dev/null

# 2. The audio driver, then coreaudiod restarts to load it.
mkdir -p $work/audio $work/scripts
ditto "$driver" "$work/audio/${driver:t}"
component $work/audio $work/audio.plist
cat > $work/scripts/postinstall <<'EOF'
#!/bin/sh
# Loads the new "Vertical Audio" driver: coreaudiod reads /Library/Audio/Plug-Ins/HAL at start.
launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod 2>/dev/null || true
exit 0
EOF
chmod 755 $work/scripts/postinstall
pkgbuild --root $work/audio --component-plist $work/audio.plist --scripts $work/scripts \
  --identifier "$driver_id.pkg" --version "$version" --install-location /Library/Audio/Plug-Ins/HAL \
  $work/audio.pkg >/dev/null

productbuild --package $work/camera.pkg --package $work/audio.pkg --sign "$ident" --timestamp "$out" >/dev/null

xcrun notarytool submit "$out" --keychain-profile "$notary" --wait
xcrun stapler staple "$out"
spctl --assess --type install --verbose=2 "$out"
