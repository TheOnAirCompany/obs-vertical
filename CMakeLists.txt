cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(ENABLE_BUILD_LEGACY "Enable legacy build" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON AUTOMOC_COMPILER_PREDEFINES OFF
  )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/core/json.cpp
    src/core/json.hpp
    src/core/destination.cpp
    src/core/destination.hpp
    src/core/encoders.cpp
    src/core/encoders.hpp
    src/core/canvas-size.cpp
    src/core/canvas-size.hpp
    src/core/scene-links.cpp
    src/core/scene-links.hpp
    src/core/render-gate.hpp
    src/core/recording.cpp
    src/core/recording.hpp
    src/core/settings.cpp
    src/core/settings.hpp
    src/obs/text.hpp
    src/obs/vertical.cpp
    src/obs/vertical.hpp
    src/obs/outputs.cpp
    src/obs/outputs.hpp
    src/obs/hotkeys.cpp
    src/obs/hotkeys.hpp
    src/obs/api.cpp
    src/obs/api.hpp
    src/ui/preview.cpp
    src/ui/preview.hpp
    src/ui/dock.cpp
    src/ui/dock.hpp
    src/ui/settings-dialog.cpp
    src/ui/settings-dialog.hpp
    src/ui/destination-dialog.cpp
    src/ui/destination-dialog.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# 9:16 virtual camera output (macOS). It needs the Vertical Camera extension,
# built separately (camera-extension/, see docs/VIRTUAL-CAMERA.md).
option(VERTICAL_VIRTUALCAM "Build the 9:16 virtual camera output (macOS)" ON)
option(VERTICAL_BUILD_CAMERA "Build the Vertical Camera extension and its host app (Xcode)" OFF)
if(APPLE AND VERTICAL_VIRTUALCAM)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/obs/virtualcam-mac.mm src/obs/virtualcam.hpp)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE VERTICAL_VIRTUALCAM)
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      "$<LINK_LIBRARY:FRAMEWORK,CoreMediaIO.framework>"
      "$<LINK_LIBRARY:FRAMEWORK,CoreMedia.framework>"
      "$<LINK_LIBRARY:FRAMEWORK,CoreVideo.framework>"
      "$<LINK_LIBRARY:FRAMEWORK,Foundation.framework>"
      "$<LINK_LIBRARY:FRAMEWORK,AppKit.framework>"
  )
endif()
if(APPLE AND VERTICAL_BUILD_CAMERA)
  add_subdirectory(camera-extension)
endif()
