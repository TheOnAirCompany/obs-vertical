#!/bin/zsh
# Packages a signed and notarized "Vertical Camera.app" into a .pkg that installs it into
# /Applications (macOS activates camera extensions only from there), signs the .pkg with
# the Developer ID Installer identity, then notarizes and staples it.
#
#   camera-extension/scripts/make-pkg.sh "build_camera/Release/Vertical Camera.app" \
#     obs-vertical-camera-0.1.0-macos-universal.pkg
#
# Environment:
#   INSTALLER_IDENT  default: Developer ID Installer: Rise (Apps) (562LS75A65)
#   NOTARY_PROFILE   notarytool keychain profile (default: rise-notary)
set -euo pipefail

app=${1:?path to Vertical Camera.app}; app=${app:a}
out=${2:?output .pkg}; out=${out:a}
ident=${INSTALLER_IDENT:-"Developer ID Installer: Rise (Apps) (562LS75A65)"}
notary=${NOTARY_PROFILE:-rise-notary}
bundle_id=$(defaults read "$app/Contents/Info" CFBundleIdentifier)
version=$(defaults read "$app/Contents/Info" CFBundleShortVersionString)

spctl --assess --type execute "$app" || { print -u2 "sign and notarize the app first (sign-and-notarize.sh)"; exit 1 }

work=$(mktemp -d)
trap 'rm -rf $work' EXIT
mkdir -p $work/root
ditto "$app" "$work/root/${app:t}"

# Install exactly into /Applications: never relocate onto another copy found elsewhere.
pkgbuild --analyze --root $work/root $work/components.plist >/dev/null
plutil -replace 0.BundleIsRelocatable -bool NO $work/components.plist
plutil -replace 0.BundleOverwriteAction -string upgrade $work/components.plist

pkgbuild --root $work/root --component-plist $work/components.plist \
  --identifier "$bundle_id.pkg" --version "$version" --install-location /Applications \
  $work/camera.pkg >/dev/null
productbuild --package $work/camera.pkg --sign "$ident" --timestamp "$out" >/dev/null

xcrun notarytool submit "$out" --keychain-profile "$notary" --wait
xcrun stapler staple "$out"
spctl --assess --type install --verbose=2 "$out"
