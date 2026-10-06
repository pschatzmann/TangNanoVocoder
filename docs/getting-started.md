# Getting Started

From a PC, without an ESP32: build the bitstream, put it and the program into the board's
flash, and let the board speak.
Installing the tools (FPGA toolchains, TinyTTS checkout, Python) is described in
[installation.md](installation.md).

You need a Tang Nano 20K with a speaker on its onboard amplifier, connected over USB, and
the TinyTTS checkout next to this repository.

```bash
gateware/build.py --gowin --flash                      # bitstream with Gowin EDA (~15 min), into flash
cmake -S model -B model/build && cmake --build model/build -j
tools/tnv.py /dev/ttyUSB1 flash-image                  # program image (flow + vocoder) into flash
tools/tnv.py /dev/ttyUSB1 say "Hello world!" --verify  # speaks over I2S, checks the PCM
```

After this, the board is ready at every power-up by itself: it boots the bitstream and loads
its program from flash.

- **Serial port:** `/dev/ttyUSB1` is the second of the two serial ports that the board's
  USB connection creates. The first is JTAG.
- **What `say` does:**
  1. Makes the sentence's z_p on the PC with `vocoder_model` (the ESP32's part) and sends it.
  2. Waits until the board has computed and played it.
  3. Prints the board's compute time against the audio's length.

  With `--verify` it also compares the board's PCM with the fixed-point model's, sample by
  sample: it is bit-exact. `--wav FILE` saves the board's PCM.
- **Vocoder alone:** `tools/tnv.py --vocoder-only PORT upload` and
  `tools/tnv.py --vocoder-only PORT say ...` use the image without the flow. The PC then runs
  the flow and sends z.
- **What `flash-image` does:**
  1. Builds the program image with `vocoder_model`, unless you give one.
  2. Writes it into the board's flash at 1MB, behind the bitstream.
  3. Waits until the board, which reconfigures after the write, has loaded it.

  Run it again after a model change.
- **Without flashing:**
  - `--load` instead of `--flash` puts the bitstream into the FPGA's SRAM only.
  - `tnv.py PORT upload` puts the program into its SDRAM only (17s).

  Both are gone after a power cycle. That is handy while trying things out.

## The same with the Arduino library, on the PC

`tests/test_desktop.cpp` runs the library's `tnv::TangNanoVocoder` exactly as the
`speak_serial` example does on an ESP32. It uses the PC's serial port in place of
`Serial1`:

```bash
cmake -S . -B build -DTNV_BUILD_TESTS=ON -DTNV_BUILD_MODEL=ON -DTNV_DEVICE_PORT=/dev/ttyUSB1
cmake --build build -j
ctest --test-dir build -L device --output-on-failure    # or: build/tests/test_desktop /dev/ttyUSB1 build/model/vocoder_model
```

It does the following:
- uploads the program if the board runs a different one (with `--no-image`, it uses the
  program the board loaded from its flash instead, as the examples do);
- speaks three sentences over I2S;
- prints the board's speed;
- compares the last sentence's z_p and PCM with the model's.

Both checks pass: z_p and the PCM are identical to the model's.

`tests/test_device.cpp` (also run by `ctest -L device`) does the same with
`tnv::VocoderClient` and all of TinyTTS, as the PC tools do.

`build/tools/host/tnv_speak /dev/ttyUSB1 "any text"` (with `-DTNV_BUILD_HOST_TOOLS=ON`)
speaks from the command line.

Next: [Using the Library](using-the-library.md) on an ESP32.
