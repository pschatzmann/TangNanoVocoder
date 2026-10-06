# Phase 1: fixed-point vocoder model on the host (see ../docs/fixed-point-model.md).
#
#   cmake -S model -B model/build && cmake --build model/build -j
#   ./model/build/vocoder_model --help
#
# Needs a TinyTTS checkout (headers + research/weights.bin + research/cmudict.bin);
# by default the sibling Arduino library ../../TinyTTS.
cmake_minimum_required(VERSION 3.16)
project(tangnano_vocoder_model CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(TINYTTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../TinyTTS" CACHE PATH "TinyTTS checkout")
get_filename_component(TINYTTS_DIR "${TINYTTS_DIR}" ABSOLUTE)
if(NOT EXISTS "${TINYTTS_DIR}/src/TinyTTS/Vocoder.h")
  message(FATAL_ERROR "TinyTTS not found at ${TINYTTS_DIR} -- pass -DTINYTTS_DIR=...")
endif()

add_executable(vocoder_model main.cpp)
target_include_directories(vocoder_model PRIVATE src ../src "${TINYTTS_DIR}/src")
target_compile_definitions(vocoder_model PRIVATE TNV_TINYTTS_DIR="${TINYTTS_DIR}")
target_compile_options(vocoder_model PRIVATE -Wall -Wextra -Wno-unused-parameter)

find_package(OpenMP)
if(OpenMP_CXX_FOUND)
  target_link_libraries(vocoder_model PRIVATE OpenMP::OpenMP_CXX)
endif()
