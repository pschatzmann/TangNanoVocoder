# Gateware

The hardware for the Tang Nano 20K (GW2AR-18), in `gateware/`. It does three things:
- receives a sentence's latent z_p over SPI or UART;
- runs TinyTTS's flow and vocoder on it from SDRAM;
- plays the audio over I2S or a PWM pin.

Every block is simulated against the fixed-point model ([studies.md](studies.md#2-the-vocoder-in-fixed-point),
[studies.md](studies.md#3-the-flow-in-fixed-point)), the compute path bit for bit, and the whole chip
is simulated end to end.

**On the board** (54 MHz, built with Gowin EDA: 80% of the logic):
- **Flow and vocoder:** 0.91 x real time. The PCM read back is bit-exact with the model (all
  178,688 samples of a 4.05s sentence).
- **Vocoder alone** (image without the flow): 0.70 x real time. The PCM read back is
  bit-exact with the model, for example all 205,824 samples of a 4.7s sentence.

"Running on the board" describes the four problems that simulation didn't show.

```
 SPI / header UART / USB UART      SPI flash (program image at power-up:
            |                       vocoder_flash_boot, plays a 'P' packet)
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
        or flow unit (LayerNorm, attention)   |         I2S  /  PWM pin
        residual stream <- DMA                |
        output buffer -> DMA -----------------'
```

| Block | File |
|---|---|
| Convolution engine (Conv1d and ConvTranspose1d) | `src/vocoder_conv_engine.v` |
| Activation tile banks | `src/vocoder_act_banks.v` |
| Bias, requantize, residual, tanh | `src/vocoder_post.v` |
| Flow: LayerNorm and attention | `src/vocoder_flow_unit.v` (tables `vocoder_exp2.hex`, `vocoder_rsqrt.hex`) |
| Scheduler, tile loader, weights, residual stream, output writer | `src/vocoder_core.v` |
| SDRAM transfers (segments, row-sized bursts) | `src/vocoder_dma.v` |
| SDRAM controller (from TangNanoGPU), port B arbiter | `src/sdram_ctrl.v`, `src/vocoder_sdram_arb.v` |
| Link protocol, SPI slave, UARTs | `src/vocoder_link.v`, `vocoder_spi_slave.v`, `vocoder_uart.v` |
| Playback, audio output (I2S, sigma-delta) | `src/vocoder_playback.v`, `vocoder_audio_out.v` |
| Program image from the SPI flash at power-up | `src/vocoder_flash_boot.v` |
| Everything without clocks and pins; replies, slots | `src/vocoder_system.v` |
| Top level, PLL | `src/vocoder_top.v` |
| DSP multipliers | `src/vocoder_mul.v` |
| Pins | `constraints/tangnano20k.cst` |

## How a sentence is computed

The quantized model is exported as a **hardware image** (`vocoder_model --flow ...
--export-hw`; without `--flow`, the vocoder alone). It contains:
- the op descriptors: 116 for the flow and 102 for the vocoder;
- per-channel parameters and weights;
- the SDRAM address of every activation buffer.

It gets into SDRAM in one of two ways: from the board's SPI flash at power-up (see "Program
image in flash"), or uploaded by the host. `vocoder_core` then runs it op by op. A convolution goes
like this (LayerNorm and attention are in the next section):

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

**SDRAM layout**:
- **Order:** activations are channel-major (a channel's frames are consecutive, two 16-bit
  values per word), so every load and store is a run of bursts.
- **Sharing:** buffers are shared by liveness. The largest tensor is 1024 values per latent
  frame.
- **Sentence length:**
  - With the flow, sentences of up to 384 latent frames (4.5s) fit, in 2.08M of the 2.1M
    words.
  - The vocoder alone (`--max-frames 448`) takes 448 frames (5.2s) in 1.97M words.
- **Slots:** two latent slots and two PCM slots. The next sentence can arrive while one is
  computed, and one can be computed while the previous one plays.

## Program image in flash

The board's 8MB SPI flash holds the bitstream from offset 0 (0.9MB). It also holds the
program image at offset 0x100000 (1MB), as a record:

| Bytes | |
|---|---|
| 4 | `"TNVF"` |
| 4 | image size in bytes (u32, little endian, a multiple of 4) |
| n | the image |
| 1 | checksum: the sum of the image bytes mod 256 |

`tools/tnv.py PORT flash-image [IMAGE]` writes it with `openFPGALoader -f -o 0x100000`.
The write makes the FPGA reconfigure, and the tool then waits until the board has loaded
the image.

At power-up, after configuration, `vocoder_flash_boot` loads it:
1. **Waits** 1ms, until the SDRAM controller has initialized. Writes before that would
   overflow the link's write FIFO and lose the first words.
2. **Reads** the record with the flash's Read Data command (03h), SPI mode 0, SCLK = clk/4
   (13.5MHz).
3. **Plays** it into the link as a host upload would: `'P'`, the size, the image bytes, the
   checksum. That is one byte every 33 clocks, about the fastest SPI slave rate, which the
   link takes. So the link writes it to SDRAM, checks the checksum, and the core reads the
   header, all as for an upload.

That takes about 1s for the 1.6MB image. While it runs, the host's bytes are ignored and
the board doesn't answer. Without a record (no magic, or an implausible size), the loader
does nothing, and the board waits for an upload as before.

The flash's pins (MSPI: CS 60, SCLK 59, MOSI 61, MISO 62, WP 57, HOLD 63) become user I/O
after configuration, through the Gowin option `use_mspi_as_gpio` (set by
`build.py --gowin`). These are the pins arduino-tangnano20k uses too. The loader's unit
test is `gateware/sim/run_flash_boot_test.sh`, against a flash model.

## The flow: LayerNorm and attention

With `vocoder_model --flow ... --export-hw`, the image starts with the flow's 116 ops, then
the vocoder's. The flow's arithmetic is in [studies.md](studies.md#3-the-flow-in-fixed-point); its
integer definitions are in `model/src/FlowOps.h`, shared by the model and `HwSim`. The ops
divide up as follows:

- **Convolutions** (projections, feed-forward, ReLU as leaky ReLU with slope 0) run on the
  engine like the vocoder's.
- **Residual adds into the latent** use descriptor flags 28/29: the output or residual
  address is relative to the current latent slot.
- **The coupling's channel flips** are folded into the weights.
- **LayerNorm and attention** are op classes 1 and 2 (descriptor word 0, bits 6-7). They run
  on `vocoder_flow_unit`, which takes over the bank, weight RAM and parameter read ports
  while it runs. It writes its results into the output buffer, from where the core stores
  them as usual.

**LayerNorm** (per tile of up to 256 frames, 32 channels):
- Three passes per frame: the sum; u = 32x - sum and sum(u^2) on the 36x36 DSP; the outputs.
- W is normalized by shifting (8 or 1 bits per clock). The leading bits are then the
  1/sqrt table index and the 16-bit interpolation fraction.
- Then two multiplies: u x R, and the normalized value x gamma.

**Attention** (one head per run, T <= 496 frames):
- k and v are loaded transposed into the banks: frame j's 16 channels in one read, at
  address j and 512 + j.
- q goes into the weight RAM.
- The relative-position tables go into the banks at 496 and 1008 (9 rows each).

Per query row, on four lane multipliers:

| Step | Clocks |
|---|---|
| q . rel_k for the 9 relative rows, into the row buffer | 36 |
| scores q . k_j plus the relative score, keeping the maximum | 4 T |
| exp2 of (score - max) x mult >> shift, from the table | T |
| r = 2^47 / sum(e), one bit per clock | 48 |
| per group of 4 channels: weighted sum of v_j and the 9 relative steps, then requantize | 4 (T + 9) |

That is about 9 T + 170 clocks per row. Four lanes rather than 16 keep it small enough to
fit next to the engine, and cost about 0.07 x real time. The two tables are block RAM ROMs
holding entry pairs (`lut[i+1]`, `lut[i]`), so one read gives both interpolation points.

## Hardware image

32-bit little-endian words, loaded at SDRAM word 0:

| Words | |
|---|---|
| 0-15 | header: magic `TNVH`, version 1, op count, op table address, max frames, latent slot 0/1 base, latent plane, PCM slot 0/1 base, image words, z scale (float), SDRAM words used |
| 16.. | op table: 16 words per op |
| | per-channel parameters: bias (int32), then `mult | shift << 16` |
| | weights, int16, two per word, `[co][kk][ci]` |

Header word 13 describes the flow (bit 0: the image has it; exp table bits << 8, rsqrt
table bits << 16, score fraction bits << 24), word 14 is the flow's op count.

Descriptor: word 0 kind (bit 0 transposed, op class in bits 6-7: 0 convolution, 1 LayerNorm,
2 attention), flags (2 residual, 3 output with tanh, 4 input
is the latent, 5 output is PCM), `cin_log2` (bits 8-10), `stride_log2` (12-14), k (16-20),
dilation (24-26); 1: cout, padding (8-13), leaky slope Q15 (16-31); 2-7: input, output and
residual base and plane stride; 8: weights; 9: parameters; 10: input and output rate (frames
per latent frame); 11: channel group size, tile size (16-31); 12: halo a, b (int16 each):
a tile's inputs are frames `(t0 + a) >> s` to `(t0 + n - 1 + b) >> s`. Flags 28 and 29: the
output, or the residual, address is relative to the current latent slot (the flow's in-place
updates of z).

LayerNorm descriptor: word 1 channels (32) and gs (bits 8-13); 11 eps; the parameters are
gamma and beta per channel. Attention descriptor: word 1 heads, head size (8-15), window
(16-23); 2 q, 6 k, 7 v; 8 the relative tables (rel_k, then rel_v, 144 values each); 11 and
12 the score and merge multiplier with their shifts (bits 16-23, 1..47).

## Link

The same byte protocol on SPI (mode 0 slave, up to 13.5 MHz), the header UART (pins 25/26)
and the board's USB serial bridge (pins 69/70), UART 8N1 at 921600 baud:

| Host sends | |
|---|---|
| `'P'`, u32 bytes, image, checksum | program upload (only while nothing is computed or played; not needed with the image in flash) |
| `'S'`, u16 frames, frames x 32 int16, checksum | one sentence: the latent (z_p with the flow, z without), frame by frame (32 channels), in the image's z scale |
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
| `flash_cs_n`, `flash_sclk`, `flash_mosi`, `flash_miso` | 60, 59, 61, 62 | out, out, out, in | onboard SPI flash (program image at power-up) |
| `flash_wp_n`, `flash_hold_n` | 57, 63 | out | held high |

## Building and testing on the board

```bash
gateware/build.py --gowin       # Gowin EDA: gateware/build/vocoder.fs (~15 min)
gateware/build.py --gowin --load    # ... and load it into the FPGA's SRAM (until power-off)
gateware/build.py --gowin --flash   # ... or into its flash (kept across power cycles)
tools/tnv.py /dev/ttyUSB1 flash-image                 # program image into the flash (loaded at power-up)
tools/tnv.py /dev/ttyUSB1 upload                      # ... or into SDRAM only (until power-off)
tools/tnv.py /dev/ttyUSB1 say "Hello world!" --verify --wav hello.wav
tools/tnv.py /dev/ttyUSB1 status | debug | calibrate  # flags, scheduler state, SDRAM read timing
```

The two toolchains:
- **`--gowin`** runs Gowin EDA's `gw_sh` in batch mode, from `$GOWIN_HOME` or `~/gowin` (see
  [installation.md](installation.md#fpga-toolchains)). Its project and reports are in
  `gateware/build/gowin/`.
- **Without `--gowin`**, `build.py` uses the open-source toolchain that arduino-tangnano20k
  installs (`~/.arduino15/packages/nanotang/tools/oss-cad-suite-gowin/*/bin`), or
  `$TNV_TOOLS/bin`. It takes about 40 min, and the current design doesn't fit with it (see
  "Resources and timing").

Loading always uses openFPGALoader.

What `tnv.py say` does:
1. Makes the latent on the PC with `vocoder_model`: z_p, the ESP32's part, or z with
   `--vocoder-only`.
2. Sends it and waits until the board has computed and played it.
3. Prints the board's compute time against the audio's length (from `'T'`).

The options:
- **`--verify`** reads the PCM back from SDRAM and compares it with the model's, sample by
  sample. Bit-exact means the board's output has the model's quality.
- **`--wav`** saves the PCM.

The serial port is the second of the two the board's USB connection creates; the first is
JTAG.

The same from C++:
- `build/tools/host/tnv_speak PORT "text"` speaks with the library's client.
- `tests/test_desktop.cpp` (the Arduino library itself) and `tests/test_device.cpp` are
  automated tests ([getting-started.md](getting-started.md)).

## Running on the board

Four things worked in simulation but not on the chip; each is fixed in the RTL and checked
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
- **DSP signed mode** (seen with the open-source toolchain): with `ASIGN`/`BSIGN` tied to
  constant 1, `MULT18X18` multiplied unsigned on 11 of the 16 lanes (depending on
  placement). With the sign inputs driven from
  a register (0 for one clock after configuration, then 1), signed mode works on all of them
  (`gateware/test/dsp16_test_top.v`: 16 multipliers, 305 cases each). `vocoder_mul.v` does
  that. An earlier workaround ran the DSPs unsigned with an arithmetic correction, which
  cost about 110 LUTs per multiplier.

- **Timing in the flow unit**: an earlier build of the flow passed Gowin's timing analysis
  (54.0 MHz). On the board, though, a few attention rows came out 1-8 LSB off at any
  sentence length, while the RTL simulation was bit-exact. Pipeline registers on the
  flow unit's tightest paths (the softmax weights from the DSP, the exp interpolation)
  made the board bit-exact. The cause was a path with no real margin, not the logic.

Hardware test bitstreams for the pieces:
- `gateware/test/dsp_test_top.v` and `dsp16_test_top.v`: the multipliers (`tools/dsp_test.py`).
- `gateware/test/mem_test_top.v`: the activation banks (`tools/mem_test.py`).

The end-to-end tests are `tests/test_desktop.cpp` and `tests/test_device.cpp`
(`ctest -L device` with `-DTNV_DEVICE_PORT=/dev/ttyUSB1`, see
[getting-started.md](getting-started.md)).

## Simulation

```bash
gateware/sim/run_tests.sh                     # I/O blocks, and engine + post per layer (minutes)
FRAMES=4 gateware/sim/run_system_test.sh      # the whole chip on one sentence (about an hour)
MAX_TILE=32 FRAMES=4 gateware/sim/run_system_test.sh   # ... with many tiles per op
OPS=1 gateware/sim/run_gate_test.sh rtl       # port-only test, the first OPS ops
gateware/sim/run_flow_unit_test.sh            # LayerNorm and attention unit (seconds)
gateware/sim/run_flash_boot_test.sh           # the flash loader against a flash model (seconds)
FRAMES=8 gateware/sim/run_flow_system_test.sh # the flow's 116 ops on the whole chip (about 30 min)
OPS=K gateware/sim/run_flow_system_test.sh    # ... only the first K ops, checking op K's output
```

The flow unit test runs the first LayerNorm and attention (both heads) of a sentence with
the model's golden vectors (`vocoder_model --flow-dump`). By default that is 40 frames of
"Hello world!"; set `FRAMES` and `TEXT` for others (passed at 349 frames). The flow system test
compares z after the flow (the latent slot) with the model's.

Needs Icarus Verilog, which runs the whole chip at only about 700 cycles per second. The
layer tests compare engine + post with the model on 17 layers (every layer type and both
sequence edges). The system test puts the image into an SDRAM model, uploads part of it
again through `'P'`, sends a sentence over the USB UART, checks the PCM in SDRAM and as
played against the model's, and reads `'T'`. `vocoder_model --check-hw` runs the same
schedule in C++ in seconds (`--max-tile` forces many tiles per op).

None of these catch how the chip differs from the simulator (see "Running on the board").
`run_gate_test.sh gate` was meant to, on yosys's netlist. It doesn't run, because yosys ships
no simulation models for the block RAM cells it now uses (`DPB`, `DPX9B`, `SDPX9B`). Gowin's
netlist with Gowin's simulation library (`IDE/simlib`) is the candidate for that now.

## Resources and timing

**With the flow, Gowin EDA** (`build.py --gowin`), 54 MHz:

| | Used | Of |
|---|---|---|
| Logic (LUT, ALU) | 16,583 | 20,736 (80%) |
| CLS | 9,231 | 10,368 (90%) |
| Registers | 6,626 | 15,915 (42%) |
| Block RAM | 41 | 46 |
| DSP | 20.25 | 24 |

Fmax 54.1 MHz: just enough, and placement-dependent (one build without the last fixes
reached only 51.7 MHz). These paths needed a pipeline register to get there:
- the flow unit's product, rounding shift and clamp, its softmax weights (DSP -> lane), and
  its exp interpolation;
- the core's group-size x plane multiply, and the tile loader's leaky-ReLU slope;
- the engine's phase setup.

The same design with yosys + nextpnr needs about 17.1k LUT4 plus 5.4k ALU. Both share the
same logic slots, so it doesn't place.

**Vocoder only**, nextpnr-himbaechel, at 54 MHz (before the flow, with the old DSP
workaround):

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

**With the flow, measured on the board: 0.91 x real time** at 54 MHz for flow and vocoder
together (4.05s of speech in 3.68s, the engine busy 80% of it). `vocoder_model --flow ...
--check-hw` estimated 0.91 x too. Attention is the flow's largest part, at about 9 T clocks
per query row and head.

The vocoder alone:

`vocoder_model --check-hw` estimates the cycles a sentence takes, following the core's
sequence (each SDRAM burst as its length plus 13 cycles, the engine as max(k x cin, 16)
cycles per block of 16 outputs, refresh). For the evaluation sentences: **0.72 x real time
at 54 MHz** (a second of audio in 0.72s), 0.60 x at 64.8 MHz; 75% of it is the engine, 23%
SDRAM transfers the engine waits for (tile loads and writes are not overlapped with
computing yet - the obvious next speedup). **Measured on the board: 0.70 x real time**
(`'T'`, 76% with the engine busy). The whole-chip simulation of a 4-frame sentence
took 1,952,599 cycles (71% with the engine busy) against an estimate of 1.98M, so the
estimate is within a few percent. The board reports its own count with `'T'`.

The first sound comes after one sentence's compute time: the flow attends over the whole
sentence, so it needs the whole latent. After that, playback runs while the next sentence
is computed.

## Open

- **Timing margin with the flow**: 54.1 MHz against 54 MHz in Gowin's analysis; another
  placement can miss.
- **Not run on hardware yet**: the SPI link, the header UART (pins 25/26) and the PWM pin
  (all simulated; the USB serial port and I2S run on the board), and the 64.8 MHz build.
- **SDRAM read timing margin**: two of the six sample points read cleanly, measured on one
  board at room temperature. The bitstream uses the one in the middle; it does not
  calibrate itself at startup (the `'L'`/`'D'` commands would allow it).
- **Speed**: tile loads and output writes don't overlap with computing (23% of the
  vocoder's time).
- **Sentence length**: at most 384 latent frames (4.5s) with the flow, 448 (5.2s) without.
  SDRAM is 99% used with the flow; packed weights would free about 0.5MB.
