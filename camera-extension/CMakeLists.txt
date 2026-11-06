# Vertical Camera: CoreMediaIO Camera Extension + its host app (macOS 13+).
#
# Standalone:
#   cmake -S camera-extension -B build_camera -G Xcode \
#     -DCODESIGN_IDENTITY="Developer ID Application: Rise (Apps) (562LS75A65)" \
#     -DVERTICAL_CAMERA_PROFILE_HOST="Vertical Camera Developer ID" \
#     -DVERTICAL_CAMERA_PROFILE_EXT="Vertical Camera Extension Developer ID"
#   cmake --build build_camera --config Release
# From the plugin: cmake --preset macos -DVERTICAL_BUILD_CAMERA=ON
#
# Without signing variables the targets still configure and build (ad hoc signature),
# but macOS only activates a system extension signed with a Developer ID and a
# provisioning profile that grants com.apple.developer.system-extension.install.
# See docs/VIRTUAL-CAMERA.md.

cmake_minimum_required(VERSION 3.28...3.30)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(vertical-camera LANGUAGES C Swift)
else()
  enable_language(Swift)
endif()

if(NOT APPLE OR NOT CMAKE_GENERATOR STREQUAL "Xcode")
  message(WARNING "Vertical Camera: the camera extension needs macOS and the Xcode generator, skipped.")
  return()
endif()

# ---- identifiers (shared with src/obs/virtualcam.hpp) -------------------------------
set(VERTICAL_TEAM_ID "562LS75A65" CACHE STRING "Apple developer team id")
set(VERTICAL_CAMERA_APP_ID "company.theonair.vertical-camera" CACHE STRING "Bundle id of the host app")
set(VERTICAL_CAMERA_EXT_ID "company.theonair.vertical-camera.extension" CACHE STRING "Bundle id of the extension")
set(VERTICAL_CAMERA_DEVICE_UUID "D0378109-93F1-4BA1-A12A-5C7B7B9BC58E")
set(VERTICAL_CAMERA_SOURCE_UUID "E4C44F98-E06C-4F3B-8969-3EA833FB7C8C")
set(VERTICAL_CAMERA_SINK_UUID "1EDE1E2A-B0A8-4816-BE2D-67793D1904D2")

# ---- signing -------------------------------------------------------------------------
set(CODESIGN_IDENTITY "" CACHE STRING "Signing identity, e.g. 'Developer ID Application: Rise (Apps) (562LS75A65)'")
set(VERTICAL_CAMERA_PROFILE_HOST
    ""
    CACHE STRING
    "Developer ID provisioning profile of the host app: an installed profile name/UUID or a .provisionprofile path"
)
set(VERTICAL_CAMERA_PROFILE_EXT
    ""
    CACHE STRING
    "Developer ID provisioning profile of the extension: an installed profile name/UUID or a .provisionprofile path"
)

# ---- version ----------------------------------------------------------------------------
set(VERTICAL_CAMERA_VERSION "0.1.0")
set(VERTICAL_CAMERA_BUILD "1")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../buildspec.json")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../buildspec.json" _buildspec)
  string(JSON VERTICAL_CAMERA_VERSION GET "${_buildspec}" version)
endif()

set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(cmake/extension-Info.plist.in "${_gen}/extension-Info.plist" @ONLY)
configure_file(cmake/app-Info.plist.in "${_gen}/app-Info.plist" @ONLY)
configure_file(cmake/extension.entitlements.in "${_gen}/extension.entitlements" @ONLY)
configure_file(cmake/app.entitlements.in "${_gen}/app.entitlements" @ONLY)

# Applies signing settings to a target. A profile given as a file path is embedded by
# hand (Contents/embedded.provisionprofile) before Xcode signs the bundle; a profile
# given by name or UUID is resolved by Xcode among the installed profiles.
function(_vertical_camera_signing target entitlements profile)
  if(NOT CODESIGN_IDENTITY)
    # Compile check only: ad hoc signature, no entitlements (restricted entitlements
    # would require a provisioning profile). Such a build cannot be activated.
    set_target_properties(
      ${target}
      PROPERTIES XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-" XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual
    )
    return()
  endif()
  set_target_properties(
    ${target}
    PROPERTIES
      XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual
      XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${CODESIGN_IDENTITY}"
      XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${VERTICAL_TEAM_ID}"
      XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${entitlements}"
      XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES
      XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--timestamp"
      XCODE_ATTRIBUTE_CODE_SIGN_INJECT_BASE_ENTITLEMENTS NO
  )
  if(profile)
    if(EXISTS "${profile}")
      add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
          "${CMAKE_COMMAND}" -E copy "${profile}" "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/embedded.provisionprofile"
        COMMENT "Embedding provisioning profile in ${target}"
      )
    else()
      set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_PROVISIONING_PROFILE_SPECIFIER "${profile}")
    endif()
  endif()
endfunction()

# ---- the camera extension ------------------------------------------------------------
add_executable(
  vertical-camera-extension
  extension/main.swift
  extension/ProviderSource.swift
  extension/DeviceSource.swift
  extension/StreamSource.swift
  extension/StreamSink.swift
)
set_target_properties(
  vertical-camera-extension
  PROPERTIES
    OUTPUT_NAME "${VERTICAL_CAMERA_EXT_ID}"
    MACOSX_BUNDLE ON
    BUNDLE_EXTENSION systemextension
    MACOSX_BUNDLE_INFO_PLIST "${_gen}/extension-Info.plist"
    XCODE_PRODUCT_TYPE com.apple.product-type.system-extension
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${VERTICAL_CAMERA_EXT_ID}"
    XCODE_ATTRIBUTE_PRODUCT_NAME "${VERTICAL_CAMERA_EXT_ID}"
    XCODE_ATTRIBUTE_SWIFT_VERSION 5.0
    XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET 13.0
    XCODE_ATTRIBUTE_SKIP_INSTALL YES
)
target_link_libraries(
  vertical-camera-extension
  PRIVATE "-framework CoreMediaIO" "-framework CoreMedia" "-framework CoreVideo" "-framework VideoToolbox"
          "-framework AppKit" "-framework IOKit"
)
_vertical_camera_signing(vertical-camera-extension "${_gen}/extension.entitlements" "${VERTICAL_CAMERA_PROFILE_EXT}")

# ---- the host app (activates the extension) -----------------------------------------
add_executable(vertical-camera-app MACOSX_BUNDLE app/main.swift app/App.swift app/Vertical.icns)
set_source_files_properties(app/Vertical.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_target_properties(
  vertical-camera-app
  PROPERTIES
    OUTPUT_NAME "Vertical Camera"
    MACOSX_BUNDLE_INFO_PLIST "${_gen}/app-Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${VERTICAL_CAMERA_APP_ID}"
    XCODE_ATTRIBUTE_SWIFT_VERSION 5.0
    XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET 13.0
)
target_link_libraries(vertical-camera-app PRIVATE "-framework AppKit" "-framework SystemExtensions")
add_dependencies(vertical-camera-app vertical-camera-extension)

# Embed the extension: Contents/Library/SystemExtensions/<bundle id>.systemextension.
# Runs before Xcode signs the app, so the app signature seals the embedded extension.
add_custom_command(
  TARGET vertical-camera-app
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:vertical-camera-app>/Library/SystemExtensions"
  COMMAND
    "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:vertical-camera-extension>"
    "$<TARGET_BUNDLE_CONTENT_DIR:vertical-camera-app>/Library/SystemExtensions/$<TARGET_BUNDLE_DIR_NAME:vertical-camera-extension>"
  COMMENT "Embedding the Vertical Camera extension in the host app"
)
_vertical_camera_signing(vertical-camera-app "${_gen}/app.entitlements" "${VERTICAL_CAMERA_PROFILE_HOST}")
