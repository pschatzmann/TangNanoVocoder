# Phase 1: fixed-point vocoder model

`model/` is a host C++ model of exactly the integer arithmetic the gateware will do, run
side by side with a float version of the same layer program and with TinyTTS's own float
vocoder. It answers the phase 1 question: **INT8 activations are not good enough; 16-bit
activations are.**

## Build and run

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

Latents come from real sentences through TinyTTS's own G2P, encoder, duration predictor and
flow (`model/src/Latent.h`, the same steps as `TinyTTSCore::synthesize()` before the vocoder
call - this is also the ESP32 half for phase 5). Activation ranges are calibrated on 12
sentences (38s of audio) and the results below are measured on 8 different ones (27s).

## Results

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

## The arithmetic

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

## Program file (`--export`)

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

The recommended configuration is 287 KB, of which 270 KB are weights. The layout is the
model's (gather order); phase 3 decides what the gateware actually wants and the exporter
follows.

## Golden vectors (`--dump DIR`)

For the first evaluated sentence: `input_z.bin` and `opNNN.bin` for every op's output, raw
int16 `[frames][channels]`, and `manifest.txt` with one line per file:
`file op_index buffer_name frames channels bits`. That's what phase 4's testbenches
compare against bit for bit. About 50 MB for a 3.5s sentence; pass `--text` for a shorter
one.

## Consequences for phase 3

- **Multipliers**: 16-bit activations x 8-bit weights (12-bit for upsampling). That's one
  18x18 multiplier per MAC instead of half of one for INT8, so the MAC budget roughly
  halves - still several times the 440M MAC/s needed, on paper.
- **Memory**: activations take 2 bytes. The largest tensors (stage 1-4, 1,024 values per
  latent frame) are 176 KB per second of audio each, so whole-sentence tensors live in
  SDRAM, as planned. SDRAM bandwidth per layer is the thing to check.
- **Requantize**: 32 x 16-bit multiply and a shift, once per output value, not per MAC.
- **Open**: the percentile and headroom options show no cheap way back to 8 bits. Finer
  scale groups (per channel, or per resblock with a rescale on the residual input) might
  give a few dB at 12 bits, but would not reach 8.
