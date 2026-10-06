# Background: the numbers behind the proposal

Everything here was measured or computed in October 2026 while porting TinyTTS to the
Tang Nano 20K (see TinyTTS's `docs/tangnano20k.md` and `docs/performance.md`).

## How much work the model is

Multiply-accumulates (MACs) per second of audio, from the shipped model's weight shapes
(44.1kHz output, 512 samples per mel frame, so 86 frames/s into the vocoder):

| Part | Channels | Frames/s | M MAC per second of audio |
|---|---|---|---|
| vocoder stage 0 | 32 | 689 | 92 |
| vocoder stage 1 | 16 | 5,512 | 184 |
| vocoder stage 2 | 8 | 11,025 | 95 |
| vocoder stage 3 | 4 | 22,050 | 45 |
| vocoder stage 4 | 2 | 44,100 | 23 |
| **vocoder total** (incl. upsampling, conv_pre/post) | | | **about 440** |
| flow (linear and conv layers) | | | about 48, plus attention, which grows with the square of the sentence length |
| text encoder, duration predictor | | per phoneme | small |

Each vocoder stage is an upsampling transposed convolution followed by three residual
blocks (kernel sizes 3, 7, 11), each with three pairs of dilated convolutions - 18
convolutions per stage, with leaky ReLU before each and residual adds.

Vocoder weights, already INT8 with one scale per output row in TinyTTS
(`default_weights_data.h`, dtype 2): about 225KB in total - stage 0's residual blocks 129KB,
stage 1's 32KB, the first upsampling layer 32KB, conv_pre 14KB, the rest small. The largest
single convolution is 11KB (32x32x11), the first upsampling layer 32KB (64x32x16).

## What the hardware can do

- **picorv32 at 54MHz** (arduino-tangnano20k, Tools > Clock Speed: Overclocked): roughly 4
  clocks per instruction, about 13 million instructions/s, no FPU (every float operation is
  a libgcc routine of hundreds of cycles). Even at one instruction per MAC that's 35x short
  of the vocoder; a realistic INT8 loop is several instructions per MAC.
- **The current INT8 engine** (`gateware/src/ai_accel_bus.v`): 8 rows x 4 lanes = 32 MACs
  per clock, about 1.7G MAC/s at 54MHz - 4x the vocoder. It's fed by the CPU one bus write
  per activation byte and two register accesses per result, so in practice it is idle most
  of the time (see "Measured on the Tang Nano" below).
- **GW2AR-18 block RAM**: 46 blocks of 18Kbit (about 103KB). With the accelerator enabled
  the current core uses 41 of them, most for the CPU's 64KB SRAM. The vocoder's weights
  don't fit on chip at once; they have to be streamed per layer from the 8MB
  SDRAM.
- **Timing**: the current design with the accelerator routes at 64-70MHz post-route, so
  54MHz has margin. nextpnr isn't given the real clock by default (it checks against
  12MHz); pass `--freq` when evaluating a new design.

## Measured on the Tang Nano (TinyTTS, `speak("Hello world!")`)

Boot Mode: Flash + SDRAM, 54MHz, Hardware Multiply/Divide, AI Accelerator. Each column
adds one round of optimization:

| Stage | Working engine | + SRAM, faster rescale/quantize | + Q24 rescale | + integer quantize, attention |
|---|---|---|---|---|
| G2P | 189s | 189s | 189s | 189s |
| encoder | 14.9s | 13.0s | 12.2s | 6.5s |
| duration_predictor | 18.4s | 16.1s | 15.4s | 10.0s |
| flow | 818s | 712s | 682s | 226s |
| vocoder | 4,949s | 3,364s | 1,852s | 1,586s |
| **total** | **100 min** | **72 min** | **46 min** | **34 min** |

In the last run the engine's own `compute()` calls took 81s of the 34 minutes; the rest of
the INT8 time went into the CPU packing windows, reading results and running the layers too
small for the engine. That is the core reason this project moves the whole vocoder into
gateware instead of offloading individual dot products.

## Measured on the ESP32 (TinyTTS `docs/performance.md`)

`speak("Hello world!")`, 1.49s of audio:

| Stage | ESP32-S3 (240MHz, optimized) | ESP32-P4 (400MHz) |
|---|---|---|
| text encoder | about 0.08-0.12s | 0.06s |
| duration predictor | about 0.13s | 0.12s |
| flow | 3.4-4.0s | 2.4s |
| vocoder | about 30s | 25.6s |
| total | about 34s | 28.4s |

For the split, the ESP32 keeps everything but the vocoder: about 3.7s (S3) or 2.6s (P4) for
1.5s of audio. These numbers predate TinyTTS's windowed attention, which removes about half
of attention's work for long inputs.

## Why this split, and not another one

- **Data on the link.** Between flow and vocoder the model carries 32 channels x 86
  frames/s: under 3KB/s as INT8, 11KB/s as float. Audio out of the vocoder is 88KB/s at
  16-bit 44.1kHz - if the FPGA plays it through the onboard amplifier, it never crosses the
  link at all.
- **Offloading dot products over SPI** (how NanoTangAI attached the engine to a second
  board) needs the activation windows of every engine call on the link: for the vocoder's
  first two stages alone that's in the order of 30MB/s (for example stage 1: 5,512 frames/s x
  2 tiles x 7 taps x 16 bytes x 18 convolutions, about 22MB/s), several times what SPI
  between these boards carries, before even reading the results back.
- **Running the whole model on the FPGA** would need the text encoder, duration predictor,
  attention, softmax and G2P in hardware or on the picorv32, which is far too slow for the
  float parts (the CPU took 226s for the flow in the best run). The ESP32 does those
  comparatively well.

## Alternatives considered

- **Pre-synthesized phrases**: synthesize on a PC, store compressed in the Tang Nano's 8MB
  flash, play with the core's helix MP3 decoder. Real time today with full quality, but a
  fixed vocabulary only.
- **Classic synthesis** (formant/LPC/diphone, e.g. the existing arduino-SAM, arduino-flite
  and arduino-espeak-ng ports): arbitrary text in real time on small CPUs, robotic quality.
- **A smaller model**: retraining at 16kHz cuts the vocoder about 2.8x; an iSTFT-based
  vocoder (iSTFTNet/Vocos style) replaces the high-rate upsampling stages with an inverse
  FFT and cuts it another 3-10x - roughly 30-60M MAC/s in total. That might bring an ESP32
  close to real time on its own, without any FPGA work, but needs retraining (TinyTTS's
  `research/` tooling) and a GPU. It's also complementary: a smaller vocoder makes this
  project's gateware smaller.
