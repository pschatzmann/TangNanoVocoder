# Repository Layout

| Directory | Contents |
|---|---|
| `src/` | The Arduino library, header-only: the link client, transports, TinyTTS's front end, the data headers (`TangNanoVocoder/data/`: program image, front-end weights) |
| `examples/` | ESP32-S3 sketches: `speak_spi`, `speak_serial` (program in the FPGA's flash), `speak_upload` (program in the sketch, with its partition table) |
| `gateware/src/` | The FPGA design (Verilog) and its tables (`*.hex`) |
| `gateware/sim/` | Testbenches and run scripts (Icarus Verilog) |
| `gateware/test/` | Small hardware test designs: DSP multipliers, activation banks |
| `gateware/constraints/` | Pin constraints for the Tang Nano 20K |
| `gateware/build.py` | Bitstream build: open-source toolchain, or Gowin EDA with `--gowin` |
| `model/` | The fixed-point model (`vocoder_model`): calibration, quality, hardware image export, `--check-hw`, the flow study (`--flow`) |
| `tools/` | `tnv.py` (PC client: upload, speak, verify, calibrate), `make_image_header.py`, `make_frontend_weights.py`, hardware test scripts, `host/tnv_speak` (C++ client) |
| `tests/` | Host tests of the client (`test_client.cpp`); on the board: the Arduino library from a PC (`test_desktop.cpp`) and the client with TinyTTS (`test_device.cpp`) |
| `docs/` | The documentation (see the README's Documentation chapter) |

## Source files

| Area | Files |
|---|---|
| Library entry points | `src/TangNanoVocoder.h` (link only), `src/TangNanoVocoderTTS.h` (with TinyTTS) |
| Client and transports | `src/TangNanoVocoder/VocoderClient.h`, `Transports.h`, `PosixSerialTransport.h` |
| Text to latent | `src/TangNanoVocoder/FrontEnd.h` (G2P, text encoder, duration predictor), `Latent.h`, `TangNanoVocoder.h` (the TTS class) |
| Fixed-point vocoder | `model/src/Program.h`, `Executors.h`, `Quantize.h`, `Export.h` |
| Fixed-point flow | `model/src/FlowFixed.h`, `FlowOps.h` (the integer LayerNorm and attention) |
| Hardware image and its interpreter | `model/src/HwImage.h`, `HwSim.h` |
| Gateware blocks | see [gateware.md](gateware.md) |
