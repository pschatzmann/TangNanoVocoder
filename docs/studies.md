# Studies

The measurements and fixed-point studies behind the design, in the order they were made.
Each one answered a question before the next step cost real effort. What was built from
them is in [gateware.md](gateware.md), and where the project stands is in
[plan-status.md](plan-status.md).

1. [Starting point](#1-starting-point): how much work TinyTTS is, what the boards did with it,
   and why the model is split the way it is.
2. [The vocoder in fixed point](#2-the-vocoder-in-fixed-point): INT8 or 16-bit activations,
   and which weight widths.
3. [The flow in fixed point](#3-the-flow-in-fixed-point): can the flow move to the FPGA too.
4. [The model tool](#4-the-model-tool-vocoder_model): `vocoder_model`, which ran these studies
   and exports the FPGA's program.

## 1. Starting point

Everything in this chapter was measured or computed in October 2026 while porting TinyTTS to
the Tang Nano 20K (see TinyTTS's `docs/tangnano20k.md` and `docs/performance.md`), before the
gateware existed.

### How much work the model is

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

### What the hardware can do

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

### Measured on the Tang Nano (TinyTTS, `speak("Hello world!")`)

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

### Measured on the ESP32 (TinyTTS `docs/performance.md`)

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

### Why this split, and not another one

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
  attention, softmax and G2P in hardware or on the picorv32. The picorv32 is far too slow
  for the float parts (the CPU took 226s for the flow in the best run). The ESP32 does those
  comparatively well.

  *Later:* the gateware got a LayerNorm and attention unit. The flow now runs on the FPGA
  (chapter 3), and the text encoder and duration predictor
  are planned next ([plan-status.md](plan-status.md), phase 6). G2P stays on the
  microcontroller.

### Alternatives considered

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

## 2. The vocoder in fixed point

The project's first question (phase 1): can the vocoder run in the integer arithmetic an FPGA
does well, and with which widths? The fixed-point model (chapter 4) runs exactly that
arithmetic side by side with a float version of the same layer program and with TinyTTS's
own float vocoder. The answer: **INT8 activations are not good enough; 16-bit activations
are.**

Latents come from real sentences through TinyTTS's own G2P, encoder, duration predictor and
flow (`src/TangNanoVocoder/Latent.h`, the same steps as `TinyTTSCore::synthesize()` before
the vocoder call). Activation ranges are calibrated on 12 sentences (38s of audio), and the
results below are measured on 8 different ones (27s).

### Results

SNR of the fixed-point model's audio against TinyTTS's float vocoder (which already uses
TinyTTS's INT8 weights), over all evaluation sentences:

| Activations | Latent on the link | Upsampling weights | Ranges | SNR | Widest accumulator |
|---|---|---|---|---|---|
| 8 bit | 8 bit | 8 bit | max | 0.2 dB | 20 bits |
| 8 bit | 8 bit | 8 bit | 99.99th percentile | 2.6 dB | 20 bits |
| 8 bit | 8 bit | 8 bit | 99.9th percentile | 5.1 dB | 20 bits |
| 8 bit | 16 bit | 16 bit | max | 0.2 dB | 27 bits |
| 10 bit | 16 bit | 16 bit | max | 8.1 dB | 27 bits |
| 12 bit | 16 bit | 16 bit | max | 19.0 dB | 29 bits |
| 16 bit | 8 bit | 16 bit | max | 17.1 dB | 33 bits |
| 16 bit | 16 bit | 8 bit | max | 24.3 dB | 28 bits |
| 16 bit | 16 bit | 10 bit | max | 35.9 dB | 28 bits |
| 16 bit | 16 bit | 16 bit | max | 38.8 dB | 33 bits |
| 16 bit | 16 bit | 16 bit | max x 2 | 37.7 dB | 32 bits |
| **16 bit** | **16 bit** | **12 bit** | **max** | **38.9 dB** | **29 bits** |

Per sentence, the recommended configuration ranges from 33.9 to 45.2 dB. For comparison,
TinyTTS's own INT8 mode (dynamic per-timestep activation scales, float upsampling) measures
21.7 dB against the same reference on the same sentences (20.2 to 25.2 dB per sentence). The
float version of the layer program matches TinyTTS's vocoder at 114 dB, so the graph is the
same.

SNR is a coarse measure for a GAN vocoder. Listen to the `--wav` output (`eN_float.wav`,
`eN_fixed.wav`, `eN_tinytts_int8.wav`) before relying on small differences; the large ones
in this table are unambiguous.

What the sweep shows:

- **Activations: 16 bit.** Each bit removed costs about 5 dB, and at 8 bits the output is
  noise. With one static scale per tensor, a tensor's rare peaks set the scale for all its
  ordinary values. The residual streams have ranges of 100-500 while most values are small.
  Clipping the peaks (percentile ranges) doesn't rescue it: it buys 5 dB, and clipping
  more saturates too many values. TinyTTS's INT8 mode works because it picks a new scale for
  every timestep, which static hardware scales cannot.
- **Latent: 16 bit.** 8 bits on the link costs 22 dB at the output: the vocoder amplifies
  input error. At 16 bits, 32 channels x 86 frames/s is 5.5 KB/s, still trivial for SPI.
- **Upsampling weights: 12 bit.** TinyTTS stores ConvTranspose1d weights with one INT8 scale
  per *input* channel, but the scale has to fold into the per-output-channel requantize
  step. Requantizing them to INT8 per output channel loses 45 dB at the first upsampling
  layer alone. 12 bits makes that loss negligible; they're under 5% of the MACs.
  Conv1d weights stay exactly TinyTTS's INT8 values.
- **Accumulators fit 32 bits** (29 bits used with 12-bit upsampling weights; 16-bit ones
  would need 33).
- **Saturation**: 2.6 outputs per million, nearly all at stage 0's resblock average.
  `--headroom 2` removes it at a cost of 1 dB: not worth it.

### The arithmetic

The vocoder is a list of 102 ops (`model/src/Program.h`): conv_pre, then per stage one
transposed convolution, 18 convolutions (3 resblocks x 3 dilated pairs) and the resblock
average, then conv_post. Each op reads and writes whole tensors, `[frames][channels]`.

Every tensor holds signed integers of the activation width with a static scale (real value =
integer x scale), shared within a **scale group**: per stage one group for the residual
stream (upsampling output and every residual add, so adds are plain integer adds), one per
first conv of each pair, one per stage output; plus conv_pre, the latent, and PCM. 58 groups
in total.

A convolution output (t, co):

1. input `x`; for negative values `x = (x * lr_mul + 2^14) >> 15` (leaky ReLU, Q15 slope:
   3277 for 0.1, 328 for conv_post's 0.01)
2. `acc = bias_q[co] + sum(w_q * x)` over taps and input channels, zero outside the
   sequence. A transposed convolution is computed as a gather too: output t reads input
   `ti` for each tap `kk` with `ti * stride - padding + kk == t` (2 taps for every
   upsampling layer).
3. requantize: `y = (acc * mult[co] + 2^(shift-1)) >> shift`, `mult` in [2^14, 2^15)
   (15 significant bits for every channel), clamp to the activation range
4. residual ops: `y = clamp(y + residual)`

The resblock average is `(a + b + c)` requantized the same way (the 1/3 is in `mult`). The
output op requantizes conv_post to Q12, then tanh by a 257-entry table over [0, 8) with
linear interpolation (about 1 LSB error) to 16-bit PCM.

Speaker conditioning is a constant per speaker and is folded into conv_pre's bias.

### Consequences for the hardware

- **Multipliers**: 16-bit activations x 8-bit weights (12-bit for upsampling). That's one
  18x18 multiplier per MAC instead of half of one for INT8, so the MAC budget roughly
  halves - still several times the 440M MAC/s needed, on paper.
- **Memory**: activations take 2 bytes. The largest tensors (stage 1-4, 1,024 values per
  latent frame) are 176 KB per second of audio each, so whole-sentence tensors live in
  SDRAM, as planned. SDRAM bandwidth per layer is the thing to check.
- **Requantize**: 32 x 16-bit multiply and a shift, once per output value, not per MAC.
- **Open** (at the time): the percentile and headroom options show no cheap way back to 8 bits. Finer
  scale groups (per channel, or per resblock with a rescale on the residual input) might
  give a few dB at 12 bits, but would not reach 8.

The gateware followed this: 16-bit activations, 8-bit conv and 12-bit upsampling weights,
36-bit accumulators, one 36x36 multiplier for the requantize step
([gateware.md](gateware.md)).

## 3. The flow in fixed point

On an ESP32-S3, TinyTTS's flow takes 3.4-4s per 1.5s of audio - the remaining obstacle to
real time once the vocoder runs on the FPGA. Could the FPGA run the flow as well? Then the
ESP32 keeps only G2P, the text encoder and the duration predictor (about 0.2s per 1.5s of
audio), and the flow moves to hardware that has time to spare (the vocoder alone runs at
0.70 x real time). The first question is the same as for the vocoder: does the flow survive
fixed point? **It does, with 12-bit weights.** It has since been built and runs on the board
([gateware.md](gateware.md#the-flow-layernorm-and-attention)).

### What the flow is

4 coupling layers (run in reverse), each: split the 32 channels into halves, project one half
16 -> 32 channels, a 3-layer transformer on it, project back 32 -> 16, subtract from the other
half; flip the channels between couplings. Each transformer layer: Q/K/V/O projections
(32 x 32), attention with 2 heads of 16 dimensions over the whole sentence plus relative
position terms (+-4 frames), residual, LayerNorm over the 32 channels; a feed-forward
32 -> 128 -> 32 with kernel 5 and ReLU, residual, LayerNorm. The speaker conditioning is added
once, before layer 2. 545K weights; about 47M multiply-accumulates per second of audio plus
attention, which grows with the square of the sentence length (about 123M per 400-frame
sentence).

### The fixed-point design

`model/src/FlowFixed.h` (with `FlowOps.h`, shared with the gateware's checks): the integer arithmetic the hardware does, next to the same
graph in float (which matches TinyTTS's flow at 134 dB):

- **Activations**: 16 bit, one static scale per tensor group, calibrated, times 1.25
  (headroom: calibration sentences don't contain every peak). The latent stream z (the
  flow's input z_p, the halves, the output) is one group.
- **Convolutions** (all projections and the feed-forward): weights symmetric per output
  channel, exact accumulators, requantized like the vocoder's; residual adds after
  requantizing into the residual's group. ReLU before clamping.
- **Softmax** in base 2: the score row minus its maximum, scaled into log2 units with 11
  fraction bits; exp2 of the fraction from a 257-entry table with linear interpolation
  (Q16), shifted by the integer part; one division per row (r = 2^47 / sum); weights
  p = e x r >> 32 in Q15; values summed with the relative-position values in v's units.
- **LayerNorm**: u = 32x - sum(x) (exact), W = sum(u^2)/32 + eps, 1/sqrt(W) from a 257-entry
  table over the mantissa in [1, 2) (times 1/sqrt(2) for odd exponents) and a power of two;
  y = u x rsqrt x gamma + beta.
- **Folded constants**: the speaker conditioning into a LayerNorm's beta, the channel flip
  into the weights, `x1 - m` into the (negated) post projection plus a residual add.

### Results

Eight evaluation sentences (27s of audio), against TinyTTS in float throughout. Waveform SNR
punishes phase differences that a GAN vocoder produces from tiny input changes and that
aren't audible, so the table also compares STFT magnitudes (both signals rounded to 16-bit
PCM, as played): spectral SNR and log-spectral distance.

| | waveform SNR | spectral SNR | log-spectral distance |
|---|---|---|---|
| TinyTTS's own INT8 vocoder (an accepted quality level) | 21.7 dB | 30.8 dB | 2.28 dB |
| float flow + fixed-point vocoder (the earlier split) | 38.9 dB | 43.1 dB | 3.95 dB |
| **fixed-point flow + fixed-point vocoder** (12-bit weights, headroom 1.25) | **32.6 dB** | **39.7 dB** | **3.97 dB** |

The flow's own output z: 65.7 dB against TinyTTS's float flow. Weight width matters a lot,
headroom little:

| Flow weights | Headroom | z | spectral SNR | saturated values | widest accumulator |
|---|---|---|---|---|---|
| 8 bit | 2 | 42.8 dB | 24.4 dB | 0 | - |
| 12 bit | 1.0 | - | 40.4 dB | 76 | 31 bits |
| **12 bit** | **1.25** | **65.7 dB** | **39.7 dB** | **2** | **31 bits** |
| 16 bit | 1.0 | - | 41.0 dB | 75 | 35 bits |
| 16 bit | 1.25 | 70.2 dB | 40.3 dB | 2 | 35 bits |

Larger softmax and LayerNorm tables (1024 entries) change nothing measurable. The vocoder
amplifies latent error strongly (z at 66 dB gives a 33 dB waveform), which is why 8-bit
weights, fine for the vocoder's convolutions, are not enough for the flow.

So the fixed-point flow costs about 3.4 dB of spectral SNR against the earlier split and adds
nothing to the log-spectral distance; it stays well above TinyTTS's own INT8 mode. Listen to
`--wav` to judge.

### What the FPGA needed

The analysis before the implementation:

- **Convolutions**: the existing engine does them all (1x1 and kernel 5, 12-bit weights as
  for the upsampling layers, accumulators 31 bits of its 36); the feed-forward's 128 output
  channels need one more bit for the channel index, and the requantize shifts reach 33 (the
  post unit takes 15-32: one more bit). ReLU is the tile loader's leaky ReLU with slope 0.
- **New units**: LayerNorm (per frame over 32 channels: two sums, a 257-entry table, three
  multiplies per value) and attention (dot products of q with k plus the relative terms,
  the row maximum, the exp2 table, one division per row, the weighted sum of v). The dot
  products could run on the engine with k and the softmax weights as data-dependent
  weights.
- **Memory**: 545K weights at 12 bits (stored as 16) add about 1.1MB to the program image,
  more than the SDRAM layout has left: the longest sentence would drop from 448 to about
  380 frames unless the buffer layout gets tighter. Program upload over the UART: about 17s
  instead of 5s.
- **Time**: about 47M MAC per second of audio for the convolutions and about 26M for
  attention at 400-frame sentences, on top of the vocoder's 440M: from 0.70 to roughly 0.80 x
  real time at 54 MHz.
- **Link**: the ESP32 sends z_p (`tnv::latentPrior()`) instead of z - the same 32 channels
  per frame, so the protocol doesn't change.

How it turned out: LayerNorm and attention became one flow unit; the dot products run on four
lane multipliers of its own rather than on the engine, to keep the design small enough to
fit. The program image is 1.6MB with sentences of up to 384 frames, the upload about 17s
over the UART, and flow and vocoder measure **0.91 x real time** on the board (16 attention
lanes would give about 0.84 x but don't fit). See
[gateware.md](gateware.md#the-flow-layernorm-and-attention).

## 4. The model tool (`vocoder_model`)

`model/` is the host C++ model behind these studies. Besides the studies' reports, it
exports the program the FPGA runs and the golden vectors the gateware is tested against.

### Build and run

```bash
cmake -S model -B model/build && cmake --build model/build -j
./model/build/vocoder_model                       # recommended configuration, prints the report
./model/build/vocoder_model --wav out/            # + WAVs: TinyTTS float, fixed point, TinyTTS INT8
./model/build/vocoder_model --export vocoder_q.bin --dump golden/
./model/build/vocoder_model --bits 8 --all-ops    # per-op table for any configuration
```

It needs a TinyTTS checkout (default: the sibling library `../TinyTTS`, or
`-DTINYTTS_DIR=...`) for its headers and `research/weights.bin`, `cmudict.bin` and
`dictionary_model.bin`. A run takes about two minutes on four cores.

The flow study (chapter 3):

```bash
./model/build/vocoder_model --flow --flow-wbits 12 --flow-headroom 1.25 [--wav DIR]
```

It calibrates the fixed-point flow on the 12 calibration sentences. Then it runs the 8
evaluation sentences three ways:
- TinyTTS in float throughout (the reference);
- the float flow with the fixed-point vocoder (the earlier split);
- the fixed-point flow with the fixed-point vocoder.

`--wav` writes `fN_float.wav`, `fN_today.wav` and `fN_fixed_flow.wav` for listening.

### Options

| Option | |
|---|---|
| `--bits N`, `--z-bits N`, `--convt-wbits N` | vocoder activation, latent and upsampling weight widths (16, 16, 12) |
| `--calib P`, `--headroom F` | ranges from the P-th percentile instead of the maximum; ranges times F |
| `--text TEXT` | evaluate this sentence instead of the built-in set (repeatable) |
| `--wav DIR` | WAVs of each evaluated sentence |
| `--all-ops` | per-op table for every op, not just stage boundaries |
| `--flow`, `--flow-wbits N`, `--flow-bits N`, `--flow-headroom F` | the flow in fixed point too (chapter 3); its weight and activation widths and headroom. The FPGA's image uses `--flow-wbits 12 --flow-headroom 1.25` |
| `--flow-exp-bits N`, `--flow-rsqrt-bits N` | softmax and LayerNorm table sizes (8: 257 entries) |
| `--export FILE`, `--dump DIR` | the quantized vocoder program and golden vectors (below) |
| `--export-hw F`, `--max-frames N` | the FPGA's program image ([gateware.md](gateware.md#hardware-image)) and the longest sentence its SDRAM layout holds (448; 384 with the flow) |
| `--check-hw`, `--max-tile N` | run the image as the scheduler does (`HwSim`) and compare with the model; force many tiles per op |
| `--sentence-out F`, `--pcm-out F`, `--z-out F`, `--crop N` | the first sentence as a link packet (z, or z_p with `--flow`), its PCM, its z after the fixed-point flow; only its first N frames |
| `--flow-dump DIR`, `--trace-op K F` | golden vectors for the flow unit test; op K's output for the flow system test |

`tools/make_image_header.py` and `tools/tnv.py` call it with the right options.


### Program file (`--export`)

Little-endian; `str` is a `u8` length plus bytes.

```
"TNVQ" u32 version (1)
u32 n_groups,  per group:  u8 bits, u8 fixed, f32 scale, str name
u32 n_buffers, per buffer: u16 channels, u16 rate (frames per latent frame), u16 group, str name
u32 input buffer, u32 output buffer
u32 n_ops, per op:
    u8 kind (0 conv, 1 transposed conv, 2 sum of 3, 3 output), u8 weight bits, u16 lr_mul
    i16 in, out, residual, in2, in3 (buffer indices, -1 = none)
    u16 cin, cout, k, dilation, stride, padding
    str name
    kinds 0, 1, 3: weights [cout][k][cin] (i8 if weight bits <= 8, else i16), i32 bias[cout]
    u16 mult[cout], u8 shift[cout]
u32 n (257), i16 tanh table[n]
```

The recommended configuration is 287 KB, of which 270 KB are weights. This was the phase 1 export, in the model's gather order. The FPGA uses the
hardware image (`--export-hw`) instead.

### Golden vectors (`--dump DIR`)

For the first evaluated sentence: `input_z.bin` and `opNNN.bin` for every op's output, raw
int16 `[frames][channels]`, and `manifest.txt` with one line per file:
`file op_index buffer_name frames channels bits`. The gateware's layer tests compare
against them bit for bit. About 50 MB for a 3.5s sentence; pass `--text` for a shorter
one.
