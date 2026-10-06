add_executable(tnv_speak tnv_speak.cpp)
target_link_libraries(tnv_speak PRIVATE TangNanoVocoder::TangNanoVocoder tinytts_headers)
target_compile_definitions(tnv_speak PRIVATE TNV_TINYTTS_DIR="${TINYTTS_DIR}")
target_compile_options(tnv_speak PRIVATE -Wall -Wextra -Wno-unused-parameter)
