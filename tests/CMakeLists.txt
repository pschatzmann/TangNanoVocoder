add_executable(test_client test_client.cpp)
target_link_libraries(test_client PRIVATE TangNanoVocoder::TangNanoVocoder tinytts_headers)
target_compile_options(test_client PRIVATE -Wall -Wextra)
add_test(NAME client COMMAND test_client)

# Hardware in the loop: sentences to a Tang Nano 20K running the vocoder
# bitstream, played over I2S. Built always, registered as a test only with
# -DTNV_DEVICE_PORT=/dev/ttyUSB1 (the board's serial port).
add_executable(test_device test_device.cpp)
# The Arduino library's TangNanoVocoder (examples/speak_serial) on the desktop,
# over the board's serial port: front end here, flow + vocoder on the FPGA.
add_executable(test_desktop test_desktop.cpp)
target_link_libraries(test_desktop PRIVATE TangNanoVocoder::TangNanoVocoder tinytts_headers)
target_compile_options(test_desktop PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_device PRIVATE TangNanoVocoder::TangNanoVocoder tinytts_headers)
target_compile_definitions(test_device PRIVATE TNV_TINYTTS_DIR="${TINYTTS_DIR}")
target_compile_options(test_device PRIVATE -Wall -Wextra -Wno-unused-parameter)
set(TNV_DEVICE_PORT "" CACHE STRING "Serial port of a Tang Nano 20K with the vocoder bitstream (enables the device test)")
if(TNV_DEVICE_PORT)
  set(_model "")
  if(TARGET vocoder_model)
    set(_model $<TARGET_FILE:vocoder_model>)
  endif()
  add_test(NAME device COMMAND test_device ${TNV_DEVICE_PORT} ${_model})
  set_tests_properties(device PROPERTIES LABELS device TIMEOUT 600)
  add_test(NAME desktop COMMAND test_desktop ${TNV_DEVICE_PORT} ${_model})
  set_tests_properties(desktop PROPERTIES LABELS device TIMEOUT 900)
endif()
