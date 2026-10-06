# TangNano Vocoder

Neural text to speech split between a microcontroller and a Sipeed Tang Nano 20K FPGA, using
[TinyTTS](https://github.com/pschatzmann/TinyTTS): the microcontroller (an ESP32) turns text
into the model's latent representation, and the FPGA runs the whole vocoder in hardware and
plays the audio itself, over I2S or a PWM pin.

On the board, the FPGA computes a sentence's audio at **0.70 x real time** (4.7s of speech in
3.3s), **bit-exact** with the fixed-point model, which matches TinyTTS's float vocoder at
39 dB SNR. The ESP32 side is written but has not run on an ESP32 yet; see [Status](#status).

## Getting started

You need a Tang Nano 20K with a speaker on its onboard amplifier, and the
[arduino-tangnano20k](https://github.com/pschatzmann/arduino-tangnano20k) core installed
(for its FPGA toolchain). From a PC, with TinyTTS checked out next to this repository:

```bash
gateware/build.py --load                                # build the bitstream (~40 min), load it
cmake -S model -B model/build && cmake --build model/build -j
tools/tnv.py /dev/ttyUSB1 upload                        # the vocoder's program image (5s)
tools/tnv.py /dev/ttyUSB1 say "Hello world!" --verify   # speaks over I2S, checks the PCM
```

`/dev/ttyUSB1` is the second of the two serial ports the board's USB connection creates.
`say` makes the latent on the PC (the ESP32's job later), sends it, waits until the board has
played it, prints the board's compute time, and with `--verify` compares the board's PCM
with the model's, sample by sample. `--flash` instead of `--load` keeps the bitstream across
power cycles; the program image has to be uploaded after every power-up.

## Using the library

This repository is an Arduino library (header only, `src/`). On an ESP32-S3 with PSRAM, with
the TinyTTS library installed and the FPGA running the bitstream:

```c++
#include "TangNanoVocoderTTS.h"
#include "TangNanoVocoder/data/default_vocoder_image.h"   // the FPGA's program, 484KB
#include "TinyTTS/data/default_weights_data.h"            // + TinyTTS's dictionary headers

tinytts::TinyTTS tts;
tnv::SPITransport vocoder_link(SPI, 10);                  // or tnv::SerialTransport(Serial1)
tnv::TangNanoVocoder vocoder(tts);

void setup() {
  SPI.begin();
  tts.setWeights(default_weights, default_weights_len);   // dictionary as in the examples
  vocoder.setImage(default_vocoder_image, default_vocoder_image_len);
  vocoder.begin(vocoder_link);                            // uploads the program if needed
  vocoder.speak("Hello world!");                          // the FPGA computes and plays it
}
```

`speak()` returns once the sentence is sent; the FPGA computes and plays it while the next
one is prepared (it holds two). One sentence may have at most 448 latent frames, about 5s of
speech. `TangNanoVocoder.h` alone is the link without TinyTTS: `tnv::VocoderClient` (status,
program upload, sentences, statistics, SDRAM readback) over `SPITransport`,
`SerialTransport` or your own `VocoderTransport`.

Examples: `examples/speak_spi` and `examples/speak_serial`, with the wiring in each. The
FPGA's pins are the same as TangNanoFaust's: SPI on 27 (SCLK), 28 (MOSI), 29 (MISO), 30 (CS),
the header UART on 25 (FPGA RX) and 26 (FPGA TX). Both examples build for an ESP32-S3 with
16MB flash and the custom partition table next to them.

**CMake**: the top-level `CMakeLists.txt` provides the INTERFACE library
`TangNanoVocoder::TangNanoVocoder`, registers as an ESP-IDF component, and builds host
targets on request:

```bash
cmake -S . -B build -DTNV_BUILD_TESTS=ON -DTNV_BUILD_HOST_TOOLS=ON -DTNV_BUILD_MODEL=ON
cmake --build build -j && ctest --test-dir build
build/tools/host/tnv_speak /dev/ttyUSB1 "Hello world!"   # speak on the board from a PC, in C++
```

With `-DTNV_DEVICE_PORT=/dev/ttyUSB1`, `ctest -L device` also runs `tests/test_device.cpp`
on the board: three sentences played over I2S, the board's speed, and its PCM compared with
the model's. Host targets need a TinyTTS checkout next to this one (or `-DTINYTTS_DIR=...`).
`tools/make_image_header.py` regenerates the program image header after model changes.

## Why

TinyTTS works on microcontrollers, but nowhere near real time:

| Platform | `speak("Hello world!")` (about 1.5s of audio) |
|---|---|
| ESP32-S3, optimized | about 34s |
| ESP32-P4, optimized | about 28s |
| Tang Nano 20K, picorv32 at 54MHz + on-chip INT8 engine, fully optimized | about 34 minutes |

Most of the model's work is the vocoder (about 90% of the multiply-accumulates, 85-90% of
the ESP32 time): about 440 million multiply-accumulates per second of audio at 44.1kHz.
Neither CPU can do that in real time. The Tang Nano's FPGA can, but only if the hardware is
fed by DMA rather than by a CPU writing every byte over its bus. That is what this project
builds.

## Architecture

```
            text
             |
   +---------v----------------------------------+
   | ESP32 (TinyTTS without its vocoder)        |
   |  G2P -> text encoder -> duration predictor |
   |  -> flow                                   |
   +---------+----------------------------------+
             | latent z: 32 channels x 86 frames/s
             | (5.5KB/s as 16 bit) over SPI or UART
   +---------v----------------------------------+
   | Tang Nano 20K FPGA                         |
   |  SPI/UART receiver -> SDRAM                |
   |  the vocoder, op by op, from SDRAM:        |
   |   input tiles in 16 block RAM banks,       |
   |   16-lane convolution engine (normal and   |
   |   transposed), weights per channel group,  |
   |   requantize, residual add, tanh -         |
   |   16-bit fixed point                       |
   |  -> I2S (onboard MAX98357A) or PWM pin     |
   +--------------------------------------------+
```

- **Only the latent crosses the link.** Audio never goes back to the ESP32. The link needs a
  few KB/s, so plain SPI or a UART is plenty. (It has to be 16 bit: an 8-bit latent costs
  22 dB at the vocoder's output.)
- **The FPGA owns the vocoder end to end**: no CPU in the inner loop - that was the
  bottleneck of the arduino-tangnano20k accelerator (in the fully optimized run its engine
  was busy for 81s of 34 minutes). The model is uploaded once as a program image of op
  descriptors and weights ([docs/gateware.md](docs/gateware.md)).
- **Latency.** The flow attends over the whole sentence, so the ESP32 must finish a sentence
  before the vocoder starts: the first sound comes after one sentence of ESP32 time plus the
  FPGA's compute time. Splitting text into sentences keeps that short; after that the FPGA
  computes the next sentence while the current one plays.
- **The ESP32 half is not real time yet.** Text encoder, duration predictor and flow took
  about 3.7s on an ESP32-S3 and 2.6s on an ESP32-P4 for 1.5s of audio - almost all of it
  the flow. They need to get about 2.5x (S3) or 1.7x (P4) faster; see phase 2.

## Plan

Each phase answers a risk before the next one costs real effort.

1. **Fixed-point vocoder model on the host** - **done**
   ([docs/fixed-point-model.md](docs/fixed-point-model.md)). A C++ model of exactly the
   hardware's arithmetic, compared with TinyTTS's float vocoder: 16-bit activations, 8-bit
   conv weights, 12-bit upsampling weights, 32-bit accumulators give 38.9 dB SNR (TinyTTS's
   own INT8 mode: 21.7 dB); INT8 activations don't work (about 0 dB). It also exports the
   hardware image and golden vectors.
2. **Flow fast enough on the ESP32** - **open**. Measure text encoder + duration predictor +
   flow on the target ESP32 with TinyTTS's current code (its windowed attention isn't in the
   numbers above). If it is still too slow: profile (`TINYTTS_PROFILE`), keep weights
   decoded in PSRAM, use the second core, or move the flow's linear layers (about 48M
   MAC/s) to the FPGA as well. If neither ESP32 gets there, the smaller-model alternative
   (docs/background.md) becomes the main path.
3. **Hardware budget** - **done**, as part of phase 4's design
   ([docs/gateware.md](docs/gateware.md)): no picorv32 - a state machine sequences the
   layers, and the design uses 63% of the logic, 37 of 46 block RAMs, 19 of 48 18x18
   multipliers and one 36x36.
4. **Gateware** - **done, running on the board** ([docs/gateware.md](docs/gateware.md)):
   convolution engine, requantize stage, scheduler with SDRAM DMA, SPI/UART input, I2S/PWM
   output, each simulated against the model bit for bit; on the board bit-exact at 0.70 x
   real time.
5. **ESP32 side** - **written, not run on an ESP32 yet**: the library in `src/` (TinyTTS up
   to the flow, the link client, SPI and UART transports) and the two examples compile for
   an ESP32-S3; the same client code runs on the board from a PC (`tnv_speak`,
   `test_device`).

## Status

The vocoder runs on a Tang Nano 20K: sentences sent from a PC over the USB serial port are
computed at 0.70 x real time at 54 MHz and played over the onboard I2S amplifier, and the
PCM read back from the board is bit-exact with the fixed-point model (205,824 of 205,824
samples of a 4.7s sentence). Getting there took three fixes that only the board showed
(SDRAM read timing, a yosys signedness issue, the DSP blocks' signed mode); see
[docs/gateware.md](docs/gateware.md#running-on-the-board).

Open:

- **ESP32**: run the examples on an ESP32-S3 wired to the board (phase 5), and measure and
  speed up its half (phase 2) - the remaining obstacle to real time end to end.
- **Not run on hardware yet**: SPI, the header UART and the PWM pin (all simulated), and the
  64.8 MHz build (passes timing; would give about 0.60 x real time).
- **SDRAM read timing margin**: two of six sample points read cleanly, measured on one board;
  the bitstream doesn't calibrate itself at startup.
- **Program image in the FPGA's flash**, instead of an upload after every power-up.
- **Speed**: overlapping SDRAM transfers with computing (23% of the FPGA's time).
- **Text splitting**: the library sends what it gets; long texts have to be split into
  sentences of at most about 5s by the caller.

The numbers behind the plan are in [docs/background.md](docs/background.md).

## Open questions

- Can an ESP32 run TinyTTS's flow in real time (phase 2), or does part of it move to the
  FPGA too?
- 44.1kHz vs. a model retrained at a lower sample rate, which cuts the vocoder's work about
  2.8x (docs/background.md, "Alternatives") - it would also free FPGA time for the flow.
- Calibrate the SDRAM read timing at every startup, or is the fixed setting reliable across
  boards and temperature?

## Repository layout

| | |
|---|---|
| `src/` | the Arduino library: link client, transports, TinyTTS latents, program image header |
| `examples/` | ESP32-S3 sketches (SPI, UART) |
| `gateware/` | the FPGA design (`src/`), simulations (`sim/`), hardware test designs (`test/`), `build.py` |
| `model/` | the fixed-point model: calibration, quality, hardware image export, `--check-hw` |
| `tools/` | `tnv.py` (PC client), `make_image_header.py`, hardware test scripts, `tnv_speak` (`host/`) |
| `tests/` | host tests of the client, and the device test |
| `docs/` | [background](docs/background.md), [fixed-point model](docs/fixed-point-model.md), [gateware](docs/gateware.md) |

## Related projects

- [TinyTTS](https://github.com/pschatzmann/TinyTTS) - the model, the ESP32 code, and
  `docs/tangnano20k.md` with the measurements on this board.
- [arduino-tangnano20k](https://github.com/pschatzmann/arduino-tangnano20k) - the Arduino
  core for the Tang Nano 20K: the FPGA toolchain this project builds with, the INT8
  dot-product accelerator this project replaces for the vocoder, and the verified pins.
- [TangNanoGPU](https://github.com/pschatzmann/TangNanoGPU) - the burst SDRAM controller
  (`gateware/src/sdram_ctrl.v`, adapted here) and the SDRAM simulation model.
- [TangNanoFaust](https://github.com/pschatzmann/NanoTangFaust) - the same SPI and UART pins
  and the PLL settings.
- [NanoTangAI](https://github.com/pschatzmann/NanoTangAI) - where the dot-product engine
  came from: an SPI-attached accelerator on a second Tang Nano 20K.
