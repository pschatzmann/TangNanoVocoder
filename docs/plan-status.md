# Plan and Status

## Plan

Each phase answers a risk before the next one costs real effort.

1. **Fixed-point vocoder model on the host**: **done**
   ([studies.md](studies.md#2-the-vocoder-in-fixed-point)). It is a C++ model of exactly the
   hardware's arithmetic, compared with TinyTTS's float vocoder:
   - 16-bit activations, 8-bit conv weights, 12-bit upsampling weights and 32-bit
     accumulators give 38.9 dB SNR. TinyTTS's own INT8 mode gives 21.7 dB.
   - INT8 activations don't work (about 0 dB).

   The model also exports the hardware image and the golden vectors.
2. **Flow fast enough for real time**: **done, the flow runs on the FPGA**. On an ESP32-S3
   the text encoder, duration predictor and flow took about 3.7s for 1.5s of audio, almost
   all of it the flow. Running the flow on the FPGA too leaves the ESP32 with G2P, text
   encoder and duration predictor (estimated 0.15 x real time):
   - **Fixed-point study**: done ([studies.md](studies.md#3-the-flow-in-fixed-point)). 12-bit
     weights cost 3.4 dB of spectral SNR against the float flow. That is still well above
     TinyTTS's own INT8 mode.
   - **Hardware**: done in simulation ([gateware.md](gateware.md#the-flow-layernorm-and-attention)).
     A LayerNorm and attention unit next to the convolution engine. The whole flow (116
     ops) runs bit-exact with the model on the simulated chip.
   - **On the board**: runs. The bitstream with the flow fits only with Gowin's own tools
     (80% of the logic, 54 MHz). Flow and vocoder run at 0.91 x real time, bit-exact with
     the model.

   The fallback, if the ESP32 has to keep the flow: profile it (`TINYTTS_PROFILE`), keep
   weights decoded in PSRAM, use the second core.
3. **Hardware budget**: **done**, as part of phase 4's design ([gateware.md](gateware.md)).
   There is no picorv32: a state machine sequences the layers.
4. **Gateware**: **done, running on the board** ([gateware.md](gateware.md)). It has:
   - the convolution engine and the requantize stage;
   - a scheduler with SDRAM DMA;
   - SPI and UART input;
   - I2S and PWM output.

   Each block is simulated against the model bit for bit. On the board it is bit-exact at
   0.70 x real time (vocoder only).
5. **ESP32 side**: **written, run from a PC, not on an ESP32 yet**. The Arduino library's
   `tnv::TangNanoVocoder` runs TinyTTS's front end (`tnv::FrontEnd`, 0.66MB of weights) and
   sends z_p; `tests/test_desktop.cpp` runs it on a PC against the board over the serial
   port. The library also has the link client and the SPI and UART transports. It and the
   two examples compile for an ESP32-S3 with 8MB flash or more.
6. **Phonemes in, audio out: ESP32 and RP2040**: **open, the next phase**. Make the FPGA run
   TinyTTS's front end too (text encoder, duration predictor, noise for z_p). The
   microcontroller then only does G2P and sends phoneme IDs. The goal is a library that runs
   in real time on an ESP32 and on an RP2040.

   Today's split doesn't work on an RP2040:

   | | ESP32-S3 | RP2040 |
   |---|---|---|
   | CPU | 240 MHz, FPU | 133 MHz Cortex-M0+, no FPU |
   | RAM | 512KB + PSRAM | 264KB, usually no PSRAM |
   | Flash | 8-16MB | 2MB (Pico) to 16MB |
   | Front end (about 30M MAC per sentence, float) | about 0.2s per 1.5s of audio (est.) | tens of seconds (software float) |

   - **Data:** the sketch's 4.3MB of data doesn't fit a Pico's flash.
   - **Fixed point:** a fixed-point port of the front end would still need roughly 1-2s per
     1.5s of audio on an RP2040.

   On the FPGA, the front end mostly reuses what is there:
   - **Text encoder:** the same transformer block as the flow (relative attention,
     LayerNorm, FFN).
   - **Duration predictor:** convolutions and norms, as on the engine.
   - **New pieces:** the phoneme embedding lookup, rounding the durations and expanding the
     frames by them, and the Gaussian noise times exp(logs_p) for z_p (an RNG and an exp
     table). They are small, mostly control logic, for the remaining 21% of the logic. That
     still needs checking.

   What the microcontroller keeps afterwards:

   | Data | Size | Notes |
   |---|---|---|
   | Pronunciation dictionary | 1.1MB | fits a 2MB Pico |
   | G2P model for unknown words | 0.97MB | optional; 4MB+ boards, or dropped on small ones |
   | Per sentence over the link | about 100 bytes of phoneme IDs | instead of 20KB of z_p |

   The library becomes G2P plus the link (plain Arduino SPI/Serial), so it runs on any board.

   Steps:
   1. **Program image in the FPGA's own flash**, loaded at power-up: **done**
      ([gateware.md](gateware.md#program-image-in-flash)). The Tang Nano 20K has
      8MB of SPI flash, and the bitstream uses less than 1MB of it. This takes the 1.6MB
      image off the microcontroller (a Pico couldn't hold it) and removes the upload after
      every power-up. It is worth doing for the ESP32 too: its sketch drops from 4.7MB to
      about 3.1MB.
   2. **Packed weights in SDRAM.** The image uses 99% of the 8MB SDRAM. Its weights are 45%
      8-bit and 54% 12-bit values stored as 16 bit; packed, the image shrinks from 1.59MB
      to about 1.03MB. That makes room for the front end's weights.
   3. **Fixed-point study** of text encoder, duration predictor and noise, as for the
      flow. The risk is the duration predictor: a small error can move a rounded phoneme
      duration by a frame and change the timing. This step decides whether the plan works.
   4. **Gateware** for the front end: the new pieces above, the ops in the program image,
      and `HwSim`, checked bit for bit as before.
   5. **Library**: a G2P-only path (text -> phoneme IDs -> FPGA) and an RP2040 example.

   Until step 4, the ESP32 keeps today's split (front end on the ESP32, z_p over the link).

## Status

**The vocoder runs on the Tang Nano 20K.** Sentences sent from a PC over the USB serial port
are computed at 0.70 x real time at 54 MHz and played over the onboard I2S amplifier. The
PCM read back from the board is bit-exact with the fixed-point model. Getting there took
three fixes that only the board showed, see
[gateware.md](gateware.md#running-on-the-board):
- SDRAM read timing;
- a yosys signedness issue;
- the DSP blocks' signed mode.

**The flow runs on the FPGA too.** It is simulated bit-exact on the whole chip, and on the
board:
- **Build:** the bitstream with it is built with Gowin EDA (`build.py --gowin`): 80% of the
  logic, 54 MHz.
- **Speed:** flow and vocoder together at 0.91 x real time, measured (4.05s of speech in
  3.68s, as estimated).
- **Quality:** bit-exact with the fixed-point model: all 178,688 samples of a 4.05s sentence,
  read back from the board. The sound is clean.
- **Sentences:** up to 384 latent frames (about 4.5s).

**The program image is in the board's flash**, so after `tnv.py flash-image` the board loads it
by itself at every power-up, in about 1s. Microcontroller sketches don't need to carry it:
`speak_spi` and `speak_serial` are 3.1MB and fit the built-in "Huge APP" partition scheme.

**The Arduino library** (`tnv::TangNanoVocoder`) runs TinyTTS's front end and sends z_p.
`tests/test_desktop.cpp` runs it unchanged on a PC against the board, over the serial port
and with I2S output, and without a program image (`--no-image`, as the examples). Its z_p
and the board's PCM are identical to the model's.

### Open

- **ESP32**: run the examples on an ESP32-S3 wired to the board (phase 5), and measure its
  part (estimated 0.15 x real time).
- **Not run on hardware yet**: SPI, the header UART and the PWM pin (all simulated), and the
  64.8 MHz build.
- **SDRAM read timing margin**: two of six sample points read cleanly, measured on one board.
  The bitstream doesn't calibrate itself at startup.
- **Speed**: overlapping SDRAM transfers with computing (23% of the vocoder's time).
- **Text splitting**: the library sends what it gets. The caller has to split long texts
  into sentences of at most about 4.5s (with the flow).

## Open questions

- Can the duration predictor run in fixed point without changing the rounded durations
  audibly (phase 6, step 3)?
- Does the front end fit in the FPGA's remaining logic (21%) and SDRAM, once the weights
  are packed?
- 44.1kHz vs. a model retrained at a lower sample rate, which cuts the vocoder's work about
  2.8x ([studies.md](studies.md#1-starting-point), "Alternatives").
- Calibrate the SDRAM read timing at every startup, or is the fixed setting reliable across
  boards and temperature?
- The open-source toolchain: can yosys's Gowin mapping get compact enough for the flow
  design, or does the project stay on Gowin EDA for it?

The numbers behind the plan are in [studies.md](studies.md#1-starting-point).
