# Gateware (phase 4)

**On the board**: a Tang Nano 20K computes sentences at 0.70 x real time at 54 MHz and plays
them over I2S; the PCM read back is bit-exact with the model (all 205,824 samples of a 4.7s
sentence). See "Running on the board" for the three problems simulation didn't show.

The vocoder's hardware for the Tang Nano 20K (GW2AR-18), in `gateware/`: it receives a
sentence's latent over SPI or UART, runs the whole vocoder from SDRAM, and plays the audio
over I2S or a PWM pin. Every block is simulated against the phase 1 model
([fixed-point-model.md](fixed-point-model.md)), the compute path bit for bit, and the whole
chip is simulated end to end.

```
 SPI / header UART / USB UART
            |
      vocoder_link ----------------------------.
   ('P' program, 'S' sentence, '?', 'T', 'R')  |
            |                                  v
            |                       sdram_ctrl (8MB SDRAM, row bursts)
            v                        ^ port B (arbiter)      ^ port A
      vocoder_core: per op, group, tile:      |              |
        DMA -> tile loader (+leaky) -> banks  |        vocoder_playback
        DMA -> weight RAM, parameter RAM      |              |
        conv engine (16 lanes) -> post        |        vocoder_audio_out
        residual stream <- DMA                |         I2S  /  PWM pin
        output buffer -> DMA -----------------'
```

| Block | File |
|---|---|
| Convolution engine (Conv1d and ConvTranspose1d) | `src/vocoder_conv_engine.v` |
| Activation tile banks | `src/vocoder_act_banks.v` |
| Bias, requantize, residual, tanh | `src/vocoder_post.v` |
| Scheduler, tile loader, weights, residual stream, output writer | `src/vocoder_core.v` |
| SDRAM transfers (segments, row-sized bursts) | `src/vocoder_dma.v` |
| SDRAM controller (from TangNanoGPU), port B arbiter | `src/sdram_ctrl.v`, `src/vocoder_sdram_arb.v` |
| Link protocol, SPI slave, UARTs | `src/vocoder_link.v`, `vocoder_spi_slave.v`, `vocoder_uart.v` |
| Playback, audio output (I2S, sigma-delta) | `src/vocoder_playback.v`, `vocoder_audio_out.v` |
| Everything without clocks and pins; replies, slots | `src/vocoder_system.v` |
| Top level, PLL | `src/vocoder_top.v` |
| DSP multipliers | `src/vocoder_mul.v` |
| Pins | `constraints/tangnano20k.cst` |

## How a sentence is computed

The quantized vocoder is exported as a **hardware image** (`vocoder_model --export-hw`):
102 op descriptors, per-channel parameters and weights, and the SDRAM address of every
activation buffer. The host uploads it once. `vocoder_core` then runs it op by op:

1. read the op's 16-word descriptor and its per-channel parameters (bias, multiplier, shift)
2. for each group of output channels whose weights fit the 8K-entry weight RAM: load them
3. for each tile of output frames (sized so the input tile fits the banks and the output
   tile fits the 8K-value output buffer):
   - load the input tile with its halo from SDRAM, one channel plane after the other, two
     values per clock, applying leaky ReLU on the way
   - run the engine; the post unit gets the residual from a stream the DMA reads meanwhile,
     and writes into the output buffer
   - write the output buffer to SDRAM, channel plane by channel plane

`model/src/HwSim.h` is the same algorithm in C++ (`vocoder_model --check-hw` runs it and
compares its PCM with the model's: bit-exact). The resblock average is a 1x1 convolution
over the three resblock outputs read as one tensor with weights 1, so the core only knows
convolutions.

**SDRAM layout**: activations are channel-major (a channel's frames are consecutive, two
16-bit values per word), so every load and store is a run of bursts. Buffers are shared by
liveness; the largest tensor is 1024 values per latent frame. With the default
`--max-frames 448`, sentences of up to 448 latent frames (5.2s) fit, in 1.97M of the 2.1M
words. Two latent slots and two PCM slots let the next sentence arrive while one is
computed, and one be computed while the previous one plays.

## Hardware image

32-bit little-endian words, loaded at SDRAM word 0:

| Words | |
|---|---|
| 0-15 | header: magic `TNVH`, version 1, op count, op table address, max frames, latent slot 0/1 base, latent plane, PCM slot 0/1 base, image words, z scale (float), SDRAM words used |
| 16.. | op table: 16 words per op |
| | per-channel parameters: bias (int32), then `mult | shift << 16` |
| | weights, int16, two per word, `[co][kk][ci]` |

Descriptor: word 0 kind (bit 0 transposed), flags (2 residual, 3 output with tanh, 4 input
is the latent, 5 output is PCM), `cin_log2` (bits 8-10), `stride_log2` (12-14), k (16-20),
dilation (24-26); 1: cout, padding (8-13), leaky slope Q15 (16-31); 2-7: input, output and
residual base and plane stride; 8: weights; 9: parameters; 10: input and output rate (frames
per latent frame); 11: channel group size, tile size (16-31); 12: halo a, b (int16 each):
a tile's inputs are frames `(t0 + a) >> s` to `(t0 + n - 1 + b) >> s`.

## Link

The same byte protocol on SPI (mode 0 slave, up to 13.5 MHz), the header UART (pins 25/26)
and the board's USB serial bridge (pins 69/70), UART 8N1 at 921600 baud:

| Host sends | |
|---|---|
| `'P'`, u32 bytes, image, checksum | program upload (once; only while nothing is computed or played) |
| `'S'`, u16 frames, frames x 32 int16, checksum | one sentence: the latent, frame by frame (32 channels), in the image's z scale |
| `'?'` | reply: status |
| `'T'` | reply: status, last sentence's cycles total and with the engine busy (u32 each), its PCM slot (u8) |
| `'R'`, u32 word address, u16 count | reply: count SDRAM words (4 bytes each), e.g. a sentence's PCM |
| `'D'` | reply: 16 debug bytes - scheduler state, op, the header as the core read it, slots (`tnv.py debug`) |
| `'L'`, u8 | SDRAM read sample point: 0-2 whole clocks, +4 half a clock earlier (`tnv.py calibrate`) |

Checksum: sum of the payload bytes mod 256. Status byte `0x80 | flags`: bit 0 READY (a
sentence may be sent), 1 RECEIVING, 2 BUSY (computing or playing), 3 ERROR (last packet
dropped: bad checksum, too long, or not ready), 4 PROGRAM (loaded). Over SPI every byte
returns the status, or the pending reply bytes after `'?'`, `'T'` or `'R'`.

## Audio output: I2S or PWM pin

A 1024-sample FIFO played at 44.1 kHz, fed from SDRAM by `vocoder_playback`:

- **I2S** for the onboard MAX98357A: Philips format, 16 bits per channel, the mono sample on
  both. BCLK (32 x fs) comes from a phase accumulator.
- **PWM pin** (pin 76): a first-order sigma-delta modulator at the full clock; add an RC
  low-pass filter.

## Pins

`gateware/constraints/tangnano20k.cst`, the same pins and settings as TangNanoFaust:

| Signal | FPGA pin | Direction | |
|---|---|---|---|
| `spi_sclk` | 27 | in | SPI clock from the MCU (header GPIO4) |
| `spi_mosi` | 28 | in | (GPIO5) |
| `spi_miso` | 29 | out | (GPIO8), tri-stated while CS is high |
| `spi_cs_n` | 30 | in | (GPIO9), active low |
| `uart_rx`, `uart_tx` | 25, 26 | in, out | header UART for an MCU (FPGA RX, FPGA TX) |
| `usb_uart_tx`, `usb_uart_rx` | 69, 70 | out, in | onboard USB-UART bridge, no wiring needed |
| `i2s_bclk`, `i2s_ws`, `i2s_din` | 56, 55, 54 | out | to the onboard MAX98357A |
| `i2s_pa_en` | 51 | out | MAX98357A SD_MODE (amplifier on) |
| `pwm_out` | 76 | out | sigma-delta audio, add an RC low-pass filter |
| `clk_27m` | 4 | in | onboard 27 MHz oscillator |
| `reset_button` | 88 | in | S1, active high |
| `leds[5:0]` | 20-15 | out | active low: program loaded, receiving, computing, playing, error, ready |

## Building and testing on the board

```bash
gateware/build.py               # bitstream: gateware/build/vocoder.fs (about 40 min)
gateware/build.py --load        # ... and load it into the FPGA's SRAM (until power-off)
gateware/build.py --flash       # ... or into its flash (kept across power cycles)
tools/tnv.py /dev/ttyUSB1 upload                      # program image, after every power-up
tools/tnv.py /dev/ttyUSB1 say "Hello world!" --verify --wav hello.wav
tools/tnv.py /dev/ttyUSB1 status | debug | calibrate  # flags, scheduler state, SDRAM read timing
```

`build.py` uses the toolchain arduino-tangnano20k installs
(`~/.arduino15/packages/nanotang/tools/oss-cad-suite-gowin/*/bin`) or `$TNV_TOOLS/bin`:
distribution yosys 0.33 maps block RAMs to cells nextpnr 0.11 can't place.

`tnv.py say` makes the latent on the PC with `vocoder_model` (TinyTTS: the ESP32's half),
sends it, waits until the board has computed and played it, and prints the board's compute
time against the audio's length (from `'T'`). `--verify` reads the PCM back from SDRAM and
compares it with the model's, sample by sample: bit-exact means the board's output has the
model's quality (38.9 dB SNR against TinyTTS's float vocoder). `--wav` saves it. The serial
port is the second of the two the board's USB connection creates (the first is JTAG).

The same from C++, with the library's own client: `build/tools/host/tnv_speak PORT "text"`,
and `tests/test_device.cpp` as an automated test (README, "Using it").

## Running on the board

Three things worked in simulation but not on the chip; each is fixed in the RTL and checked
on the board:

- **SDRAM burst reads** (`sdram_ctrl.v`, from TangNanoGPU, where bursts were never tested
  on hardware): sampling read data at the controller's original point, or one clock
  earlier, mixed bits of neighbouring words. Reads are now sampled at a selectable point,
  including half-clock steps (falling-edge capture); the default (5: half a clock earlier
  than the original) is in the middle of the window that reads cleanly. `tnv.py calibrate`
  sweeps all points (the core re-reads its header, `'D'` shows it). Single-word reads work
  at any point, so they never showed the problem.
- **Signed arrays**: yosys drops the signedness of `reg signed` arrays, so the engine's
  32-bit products were zero-extended into the 36-bit accumulators. Sign extension is
  explicit now.
- **DSP signed mode**: `MULT18X18` with `ASIGN`/`BSIGN` set multiplied unsigned on 11 of
  the 16 lanes (depending on placement). All DSPs now run unsigned, and the signed product
  is recovered arithmetically (`vocoder_mul.v`).

Hardware test bitstreams for the pieces: `gateware/test/dsp_test_top.v` (multipliers,
`tools/dsp_test.py`) and `gateware/test/mem_test_top.v` (activation banks,
`tools/mem_test.py`). The end-to-end test is `tests/test_device.cpp` (`ctest -L device`
with `-DTNV_DEVICE_PORT=/dev/ttyUSB1`, see the README).

## Simulation

```bash
gateware/sim/run_tests.sh                     # I/O blocks, and engine + post per layer (minutes)
FRAMES=4 gateware/sim/run_system_test.sh      # the whole chip on one sentence (about an hour)
MAX_TILE=32 FRAMES=4 gateware/sim/run_system_test.sh   # ... with many tiles per op
OPS=1 gateware/sim/run_gate_test.sh rtl       # port-only test, the first OPS ops
```

Needs Icarus Verilog, which runs the whole chip at only about 700 cycles per second. The
layer tests compare engine + post with the model on 17 layers (every layer type and both
sequence edges). The system test puts the image into an SDRAM model, uploads part of it
again through `'P'`, sends a sentence over the USB UART, checks the PCM in SDRAM and as
played against the model's, and reads `'T'`. `vocoder_model --check-hw` runs the same
schedule in C++ in seconds (`--max-tile` forces many tiles per op).

None of these catch how the chip differs from the simulator (see "Running on the board").
`run_gate_test.sh gate` was meant to, on yosys's netlist, but yosys ships no simulation
models for the block RAM cells it now uses (`DPB`, `DPX9B`, `SDPX9B`), so it doesn't run.

## Resources and timing

nextpnr-himbaechel, whole design at 54 MHz:

| | Used | Of |
|---|---|---|
| LUT4 | 13,257 | 20,736 (63%) |
| ALU | 4,690 | 15,552 (30%) |
| Flip-flops | 6,127 | 15,552 (39%) |
| Block RAM | 37 | 46 |
| MULT18X18 | 19 | 48 |
| MULT36X36 | 1 | 12 |
| MULT9X9 | 3 | 96 |

16 multipliers are the engine's lanes, two the tile loader's leaky ReLU, the 36x36 the post
unit's requantize; yosys maps three small multiplies (tanh interpolation) to `MULT9X9`.
nextpnr's timing analysis: 88 MHz maximum for the system clock, so the 64.8 MHz PLL
setting (`build.py --mhz 64.8`) has margin too - not tried on the board yet.

## Speed

`vocoder_model --check-hw` estimates the cycles a sentence takes, following the core's
sequence (each SDRAM burst as its length plus 13 cycles, the engine as max(k x cin, 16)
cycles per block of 16 outputs, refresh). For the evaluation sentences: **0.72 x real time
at 54 MHz** (a second of audio in 0.72s), 0.60 x at 64.8 MHz; 75% of it is the engine, 23%
SDRAM transfers the engine waits for (tile loads and writes are not overlapped with
computing yet - the obvious next speedup). **Measured on the board: 0.70 x real time**
(`'T'`, 76% with the engine busy). The whole-chip simulation of a 4-frame sentence
took 1,952,599 cycles (71% with the engine busy) against an estimate of 1.98M, so the
estimate is within a few percent. The board reports its own count with `'T'`.

The first sound comes after one sentence's compute time (the vocoder needs the whole
latent; the ESP32's flow needs the whole sentence anyway), then playback runs while the
next sentence is computed.

## Open

- **Not run on hardware yet**: the SPI link, the header UART (pins 25/26) and the PWM pin
  (all simulated; the USB serial port and I2S run on the board), and the 64.8 MHz build.
- **SDRAM read timing margin**: two of the six sample points read cleanly, measured on one
  board at room temperature. The bitstream uses the one in the middle; it does not
  calibrate itself at startup (the `'L'`/`'D'` commands would allow it).
- **Program image in flash**: it is uploaded after every power-up (484 KB, 5s over the
  USB serial port); storing it in the FPGA's flash and loading it at boot would remove that.
- **Speed**: tile loads and output writes don't overlap with computing (23% of the time).
- **Sentence length**: at most 448 latent frames (5.2s) with the default SDRAM layout.
