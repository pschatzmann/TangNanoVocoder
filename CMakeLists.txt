# TangNanoVocoder is header only (src/); this registers its include path for
#   - ESP-IDF: point EXTRA_COMPONENT_DIRS (or idf_component.yml) at this
#     repository; ESP_PLATFORM is detected below and it registers as a
#     component (TinyTTS is a separate component).
#   - plain CMake: the INTERFACE library TangNanoVocoder::TangNanoVocoder,
#     plus optional host targets:
#
#   cmake -S . -B build -DTNV_BUILD_TESTS=ON -DTNV_BUILD_HOST_TOOLS=ON -DTNV_BUILD_MODEL=ON
#   cmake --build build -j && ctest --test-dir build
#
#     TNV_BUILD_TESTS       unit tests of the link client (tests/); with
#                           -DTNV_DEVICE_PORT=/dev/ttyUSB1 also a test on a
#                           board: sentences played over I2S (ctest -L device)
#     TNV_BUILD_HOST_TOOLS  tnv_speak: text -> TinyTTS on the PC -> the
#                           board over its USB serial port (tools/host/)
#     TNV_BUILD_MODEL       vocoder_model, the fixed-point model and image
#                           export (model/)
#   Host targets need a TinyTTS checkout: TINYTTS_DIR (default ../TinyTTS).
#   The Arduino IDE doesn't use this file (library.properties).
if(ESP_PLATFORM)
  idf_component_register(INCLUDE_DIRS "src")
  return()
endif()

cmake_minimum_required(VERSION 3.16)
project(TangNanoVocoder CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(tangnano_vocoder INTERFACE)
add_library(TangNanoVocoder::TangNanoVocoder ALIAS tangnano_vocoder)
target_include_directories(tangnano_vocoder INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(tangnano_vocoder INTERFACE cxx_std_17)

option(TNV_BUILD_TESTS "Build the host unit tests (tests/)" OFF)
option(TNV_BUILD_HOST_TOOLS "Build tnv_speak, the host client for the board (needs TinyTTS)" OFF)
option(TNV_BUILD_MODEL "Build vocoder_model, the fixed-point model (needs TinyTTS)" OFF)

set(TINYTTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../TinyTTS" CACHE PATH "TinyTTS checkout (host targets)")
get_filename_component(TINYTTS_DIR "${TINYTTS_DIR}" ABSOLUTE)

if(TNV_BUILD_TESTS OR TNV_BUILD_HOST_TOOLS)
  if(NOT EXISTS "${TINYTTS_DIR}/src/TinyTTS.h")
    message(FATAL_ERROR "TinyTTS not found at ${TINYTTS_DIR} -- pass -DTINYTTS_DIR=...")
  endif()
  add_library(tinytts_headers INTERFACE)
  target_include_directories(tinytts_headers INTERFACE "${TINYTTS_DIR}/src")
endif()

if(TNV_BUILD_MODEL)
  add_subdirectory(model)
endif()

if(TNV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TNV_BUILD_HOST_TOOLS)
  add_subdirectory(tools/host)
endif()

