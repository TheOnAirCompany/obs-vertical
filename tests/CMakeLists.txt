cmake_minimum_required(VERSION 3.20)

# Unit tests of the Vertical core (src/core). Pure C++17: no OBS, no Qt.
#   cmake -S tests -B build_tests && cmake --build build_tests && ./build_tests/vertical-core-tests

project(vertical-core-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/core")

add_executable(
  vertical-core-tests
  test-main.cpp
  test-json.cpp
  test-destination.cpp
  test-encoders.cpp
  test-canvas-size.cpp
  test-scene-links.cpp
  test-render-gate.cpp
  test-recording.cpp
  test-settings.cpp
  test-dock-model.cpp
  test-audio-mix.cpp
  test-meter.cpp
  test-sync.cpp
  ${CORE_DIR}/json.cpp
  ${CORE_DIR}/destination.cpp
  ${CORE_DIR}/encoders.cpp
  ${CORE_DIR}/canvas-size.cpp
  ${CORE_DIR}/scene-links.cpp
  ${CORE_DIR}/recording.cpp
  ${CORE_DIR}/settings.cpp
  ${CORE_DIR}/dock-model.cpp
  ${CORE_DIR}/audio-mix.cpp
  ${CORE_DIR}/meter.cpp
  ${CORE_DIR}/sync.cpp
)
target_include_directories(vertical-core-tests PRIVATE "${CORE_DIR}")

if(MSVC)
  target_compile_options(vertical-core-tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(vertical-core-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME vertical-core COMMAND vertical-core-tests)

# macOS: the Vertical Audio driver, loaded by a host test that calls its
# AudioServerPlugInDriverInterface directly (nothing is installed).
if(APPLE)
  add_library(vertical-audio-driver-module MODULE ../camera-extension/audio-driver/VerticalAudio.cpp)
  target_include_directories(vertical-audio-driver-module PRIVATE "${CORE_DIR}")
  target_link_libraries(vertical-audio-driver-module PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
  target_compile_options(vertical-audio-driver-module PRIVATE -Wall -Wextra)

  add_executable(vertical-audio-driver-tests driver-host-test.cpp)
  target_include_directories(vertical-audio-driver-tests PRIVATE "${CORE_DIR}")
  target_link_libraries(vertical-audio-driver-tests PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
  target_compile_options(vertical-audio-driver-tests PRIVATE -Wall -Wextra)
  add_dependencies(vertical-audio-driver-tests vertical-audio-driver-module)
  add_test(NAME vertical-audio-driver COMMAND vertical-audio-driver-tests "$<TARGET_FILE:vertical-audio-driver-module>")
endif()
