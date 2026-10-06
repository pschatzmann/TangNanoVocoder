# Installation

There are two ways to use this repository:
- as an **Arduino library** on an ESP32 that talks to a Tang Nano 20K running the bitstream;
- as a **development environment** for the fixed-point model, the gateware and the host
  tools.

## Arduino Library

### What you need

- An **ESP32-S3 with PSRAM and at least 4MB flash**. A sketch embeds about 2.7MB of data
  (TinyTTS's front-end weights 0.66MB, the dictionary and G2P model 2.1MB), so it needs the
  built-in "Huge APP" partition scheme. `examples/speak_upload` also carries the FPGA's
  program image and needs 8MB of flash.
- A **Sipeed Tang Nano 20K** with a speaker on its onboard amplifier (I2S), or an RC
  low-pass filter and amplifier on FPGA pin 76 (PWM).
- **The FPGA's flash prepared, once from a PC** (see
  [Development Environment](#development-environment) for the tools):

  ```bash
  gateware/build.py --gowin --flash             # the bitstream
  tools/tnv.py /dev/ttyUSB1 flash-image         # the program image (flow + vocoder, 1.6MB)
  ```

  The FPGA then boots by itself at every power-up, including loading its program, in about
  2s.

### Install the libraries

1. The ESP32 Arduino core (Boards Manager: "esp32" by Espressif Systems).
2. [TinyTTS](https://github.com/pschatzmann/TinyTTS), for its G2P, text encoder and duration
   predictor code and its dictionary data.
3. This library:

```bash
cd ~/Arduino/libraries      # your sketchbook's libraries folder
git clone https://github.com/pschatzmann/TinyTTS.git
git clone https://github.com/pschatzmann/TangNanoVocoder.git
```

The library is header-only (`src/`):
- `TangNanoVocoder.h` is the link to the FPGA on its own: the client and the SPI and Serial
  transports.
- `TangNanoVocoderTTS.h` adds the text-to-speech class `tnv::TangNanoVocoder`, with
  TinyTTS's front end.

The data comes as headers in `TangNanoVocoder/data/` and TinyTTS's `TinyTTS/data/`. See
[using-the-library.md](using-the-library.md).

### Wiring

Tang Nano 20K header pins, the same as TangNanoFaust's (3.3V logic, common GND):

| Link | ESP32 | FPGA pin |
|---|---|---|
| SPI | SCK | 27 |
| | MOSI | 28 |
| | MISO | 29 |
| | CS | 30 |
| UART | TX | 25 (FPGA RX) |
| | RX | 26 (FPGA TX) |

Pick one link. The ESP32 pins are set in the sketch.

### Build an example

`examples/speak_spi`, `examples/speak_serial` or `examples/speak_upload`
([using-the-library.md](using-the-library.md#examples-and-wiring)). In the Arduino IDE (Tools
menu):

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module (or your S3 board) |
| Flash Size | what your module has (4MB or more; 8MB or more for `speak_upload`) |
| PSRAM | OPI PSRAM (or what your module has) |
| Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS); for `speak_upload`: Custom (its `partitions.csv`) |

With arduino-cli:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app examples/speak_spi
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=8M,PSRAM=opi,PartitionScheme=custom \
  examples/speak_upload
```

On startup, the sketch finds the FPGA and reads which program it runs. Then `speak()` sends
each sentence's z_p, and the FPGA computes it and plays the audio. `speak_upload` instead
uploads its own copy of the program when the FPGA doesn't run it: 1.6MB, about 17s over
the UART.

### ESP-IDF and CMake

The repository registers as an ESP-IDF component (point `EXTRA_COMPONENT_DIRS` at it or use
`idf_component.yml`). In plain CMake it is the INTERFACE library
`TangNanoVocoder::TangNanoVocoder`.

## Development Environment

For changing the model or the gateware, or running everything from a PC without an ESP32.

### Checkouts

TinyTTS must be next to this repository (or pass `-DTINYTTS_DIR=...` to CMake):

```bash
git clone https://github.com/pschatzmann/TinyTTS.git
git clone https://github.com/pschatzmann/TangNanoVocoder.git
cd TangNanoVocoder
```

### Host tools

| Tool | For |
|---|---|
| CMake 3.16+, a C++17 compiler | the fixed-point model, host tests, `tnv_speak` |
| Python 3 with `pyserial` | `tools/tnv.py` (upload, speak, verify), hardware test scripts |
| Icarus Verilog (`iverilog`, `vvp`) | gateware simulations |

```bash
cmake -S model -B model/build && cmake --build model/build -j     # vocoder_model
cmake -S . -B build -DTNV_BUILD_TESTS=ON -DTNV_BUILD_HOST_TOOLS=ON -DTNV_BUILD_MODEL=ON
cmake --build build -j && ctest --test-dir build                  # host tests
pip install pyserial
```

### FPGA toolchains

`gateware/build.py` supports two toolchains:

- **Gowin EDA** (`--gowin`): Gowin's own synthesis, place and route. **The current design
  needs it**: Gowin maps it to 80% of the logic at 54 MHz. Install
  the Gowin IDE (the Education edition works for the GW2AR-18) to `~/gowin`, or set
  `$GOWIN_HOME`. `build.py` runs its `gw_sh` in batch mode and works around the IDE's Linux
  library clashes itself:
  - it preloads the system's freetype;
  - it uses the IDE's own Qt libraries and plugins;
  - it turns off OpenGL.
- **Open source** (`build.py` without `--gowin`): yosys, nextpnr-himbaechel and gowin_pack.
  Install the [arduino-tangnano20k](https://github.com/pschatzmann/arduino-tangnano20k)
  Arduino core, which brings them along
  (`~/.arduino15/packages/nanotang/tools/oss-cad-suite-gowin/*/bin`), or set `$TNV_TOOLS`
  to a directory with `bin/`. Don't use distribution yosys 0.33: it maps block RAMs to
  cells nextpnr can't place.
  - **Limit:** yosys's mapping is much less compact than Gowin's. The current design (with
    the flow) needs about 109% of the logic, so it doesn't place.
  - **What it still builds:** the earlier vocoder-only design (63%), and the small test
    designs in `gateware/test/`.

openFPGALoader loads the bitstream into the FPGA and writes the FPGA's flash. It comes with
the arduino-tangnano20k core, so install that core for `--load`, `--flash` and `tnv.py
flash-image` even when you build with Gowin EDA. Distribution packages can be too old (v0.12
can't write data at a flash offset); `build.py` and `tnv.py` use the core's.

```bash
gateware/build.py --gowin --load     # build with Gowin (~15 min), load into SRAM (until power-off)
gateware/build.py --gowin --flash    # ... or into the FPGA's flash (kept across power cycles)
```

### First run on the board

The board's USB connection creates two serial ports. The second one (for example
`/dev/ttyUSB1`) is the vocoder's link. The first is JTAG.

```bash
tools/tnv.py /dev/ttyUSB1 flash-image                     # program image into the FPGA's flash, once
tools/tnv.py /dev/ttyUSB1 upload                          # ... or into its SDRAM only, until power-off
tools/tnv.py /dev/ttyUSB1 say "Hello world!" --verify     # speak, compare the PCM with the model
tools/tnv.py /dev/ttyUSB1 calibrate                       # SDRAM read timing check
```

See [getting-started.md](getting-started.md) for what these do.

On Linux, your user needs access to the serial ports (the `dialout` group) and a udev rule
for the board's JTAG for openFPGALoader. See the openFPGALoader documentation.

### Simulations

```bash
gateware/sim/run_tests.sh                      # I/O blocks, engine + post per layer (minutes)
gateware/sim/run_flow_unit_test.sh             # LayerNorm and attention unit (seconds)
FRAMES=8 gateware/sim/run_flow_system_test.sh  # the flow on the whole chip (about 30 min)
FRAMES=4 gateware/sim/run_system_test.sh       # the vocoder on the whole chip (about an hour)
```

The details are in [gateware.md](gateware.md).
