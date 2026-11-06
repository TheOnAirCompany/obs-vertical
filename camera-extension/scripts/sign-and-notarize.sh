#!/bin/zsh
# Re-signs "Vertical Camera.app" inside-out (and VerticalAudio.driver when given), then
# notarizes both and staples the app.
# Use it on a build made with the signing variables (see docs/VIRTUAL-CAMERA.md), or to
# sign an unsigned build by hand.
#
#   camera-extension/scripts/sign-and-notarize.sh \
#     "build_camera/Release/Vertical Camera.app" \
#     host.provisionprofile extension.provisionprofile [build_camera/Release/VerticalAudio.driver]
#
# Environment:
#   CODESIGN_IDENT   signing identity (default: Developer ID Application: Rise (Apps) (562LS75A65))
#   NOTARY_PROFILE   notarytool keychain profile (default: rise-notary)
#   ENTITLEMENTS_DIR folder with app.entitlements and extension.entitlements
#                    (default: <build dir>/generated, written by CMake)
#   SKIP_NOTARIZE=1  sign only
set -euo pipefail

app=${1:?path to Vertical Camera.app}
host_profile=${2:?host app .provisionprofile}
ext_profile=${3:?extension .provisionprofile}
driver=${4:-}
ident=${CODESIGN_IDENT:-"Developer ID Application: Rise (Apps) (562LS75A65)"}
notary=${NOTARY_PROFILE:-rise-notary}
ents=${ENTITLEMENTS_DIR:-"${app:h:h}/generated"}
ext="${app}/Contents/Library/SystemExtensions/company.theonair.vertical-camera.extension.systemextension"

[[ -d $ext ]] || { print -u2 "extension not embedded: $ext"; exit 1 }
[[ -f $ents/app.entitlements && -f $ents/extension.entitlements ]] || { print -u2 "entitlements not found in $ents"; exit 1 }
[[ -z $driver || -d $driver ]] || { print -u2 "driver not found: $driver"; exit 1 }

# 1. Profiles go in before signing: the signature seals them.
cp "$ext_profile" "$ext/Contents/embedded.provisionprofile"
cp "$host_profile" "$app/Contents/embedded.provisionprofile"

# 2. Inside-out: the extension first, then the app that contains it. No --deep.
codesign --force --timestamp --options runtime --entitlements "$ents/extension.entitlements" --sign "$ident" "$ext"
codesign --force --timestamp --options runtime --entitlements "$ents/app.entitlements" --sign "$ident" "$app"
codesign --verify --strict --deep --verbose=2 "$app"

# The audio driver: no entitlement, no profile (coreaudiod loads it as a plug-in).
if [[ -n $driver ]]; then
  codesign --force --timestamp --options runtime --sign "$ident" "$driver"
  codesign --verify --strict --verbose=2 "$driver"
fi

[[ ${SKIP_NOTARIZE:-0} == 1 ]] && exit 0

# 3. Notarize (one zip with the app and the driver), then staple the ticket to the app.
#    A driver bundle cannot be stapled: the package that installs it is (make-pkg.sh).
stage=$(mktemp -d)
ditto "$app" "$stage/${app:t}"
[[ -n $driver ]] && ditto "$driver" "$stage/${driver:t}"
zip="${app:h}/Vertical-Camera-notarize.zip"
ditto -c -k "$stage" "$zip"
rm -rf "$stage"
xcrun notarytool submit "$zip" --keychain-profile "$notary" --wait
xcrun stapler staple "$app"
spctl --assess --type execute --verbose=2 "$app"
rm -f "$zip"
