# TangNano Vocoder

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue?logo=arduino&logoColor=white)](https://www.arduino.cc/reference/en/libraries/)
[![ESP-IDF Component](https://img.shields.io/badge/ESP--IDF-component-blue?logo=espressif&logoColor=white)](idf_component.yml)
[![CMake](https://img.shields.io/badge/CMake-supported-blue?logo=cmake&logoColor=white)](CMakeLists.txt)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-blue)](https://opensource.org/licenses/Apache-2.0)

[TinyTTS](https://github.com/pschatzmann/TinyTTS) is a proof of concept: neural text to speech
running entirely on a microcontroller. It is a small VITS-style model with 1.6M parameters,
covering the whole pipeline:
- G2P: text to phonemes, with a pronunciation dictionary and a small model for unknown words;
- a text encoder and a duration predictor;
- a normalizing flow;
- a HiFi-GAN vocoder that produces 44.1kHz audio.

It is all hand-written C++ with INT8 weights. It works, and the speech is good, but it is far
too slow:

| Platform | `speak("Hello world!")` (about 1.5s of audio) |
|---|---|
| ESP32-S3, optimized | about 34s |
| ESP32-P4, optimized | about 28s |
| [Sipeed Tang Nano 20K](https://wiki.sipeed.com/hardware/en/tang/tang-nano-20k/nano-20k.html), picorv32 at 54MHz + on-chip INT8 engine, fully optimized | about 34 minutes |

The conclusion: **neural speech on microcontrollers is too slow**, by a factor of about 20
on the fastest ESP32. Most of the work is the vocoder: about 440 million multiply-accumulates
per second of audio, about 90% of the model's total. The flow is most of the rest. No
microcontroller CPU does that in real time.

This project moves the heavy part into hardware. A Sipeed Tang Nano 20K FPGA runs the flow
and the vocoder, fed by its own DMA from SDRAM rather than by a CPU.

<img src="https://wiki.sipeed.com/hardware/zh/tang/tang-nano-20k/assets/nano_20k/tang_nano_20k_3920_top.png" alt="Sipeed Tang Nano 20K" width="300">

The microcontroller keeps the light part:
- **The microcontroller** (an ESP32-S3) runs TinyTTS's front end. It turns text into
  phonemes, then into the latent z_p.
- **The FPGA** runs the flow and the vocoder, and plays the audio itself, over I2S or a PWM
  pin.

On the board, the FPGA computes flow and vocoder together at **0.91 x real time** (4.05s of
speech in 3.68s). The output is **bit-exact** with the fixed-point model, which matches
TinyTTS in float at 39.7 dB spectral SNR (the vocoder alone: 39 dB waveform SNR). The FPGA
loads its program from its own flash at power-up, so the microcontroller only sends
sentences.

The Arduino library has run this split end to end from a PC against the board
(`tests/test_desktop.cpp`), but not on an ESP32 yet. The next phase moves TinyTTS's front
end to the FPGA as well, so that the microcontroller only does G2P: then it can be an
RP2040 too. See [Plan and Status](docs/plan-status.md).

To try it, start with [Getting Started](docs/getting-started.md); all documents are listed under
[Documentation](#documentation).

## Architecture

```
            text
             |
   +---------v-----------------------------------------+
   | ESP32-S3: TinyTTS's front end (tnv::FrontEnd)     |
   |  G2P: slim CMU dictionary 1.12MB,                 |
   |       G2P model for unknown words 0.99MB          |
   |  text encoder 0.20MB -> duration predictor 0.45MB |
   |  -> sampled prior z_p  about 0.15 x real time (est.)
   +---------+-----------------------------------------+
             | z_p: 32 channels x 86 frames/s, 16 bit
             | (5.5KB/s) over SPI or UART
   +---------v-----------------------------------------+
   | Tang Nano 20K FPGA (program image 1.6MB, loaded   |
   | from its own flash at power-up; flow 116 ops +    |
   | vocoder 102 ops)                                  |
   |  SPI/UART receiver -> SDRAM (8MB)                 |
   |  op by op, from SDRAM, 16-bit fixed point:        |
   |   flow:    convolutions on the engine,            |
   |            LayerNorm and attention unit -> z      |
   |   vocoder: 16-lane convolution engine (normal and |
   |            transposed), requantize, residual, tanh|
   |                                about 0.9 x real time
   |  -> I2S (onboard MAX98357A) or PWM pin            |
   +---------------------------------------------------+
```

- **What runs where.**
  - The ESP32 keeps the light part of TinyTTS: text to phonemes, the text encoder and the
    duration predictor, with 0.66MB of weights.
  - The FPGA runs the heavy part: the flow (1.14MB of weights in TinyTTS, about 3.4s per
    1.5s of audio on an ESP32-S3) and the vocoder (440M multiply-accumulates per second of
    audio).
  - On the ESP32-S3, TinyTTS alone needs about 34s for 1.5s of audio. Split this way, the
    FPGA measures 0.91 x real time, and the ESP32's part is estimated at 0.15 x (from its
    share of the measured 3.7s for text encoder, duration predictor and flow).
- **Only the latent crosses the link.** Audio never goes back to the ESP32. The link needs a
  few KB/s, so plain SPI or a UART is plenty. (It has to be 16 bit: an 8-bit latent costs
  22 dB at the vocoder's output.)
- **The FPGA runs the model end to end, with no CPU in the inner loop.** A CPU in the loop
  was the bottleneck of the arduino-tangnano20k accelerator: in the fully optimized run, its
  engine was busy for 81s of 34 minutes. The model is a program image of op descriptors and
  weights. The FPGA loads it from its own flash at power-up, and a hardware scheduler with
  SDRAM DMA runs it ([docs/gateware.md](docs/gateware.md)). The microcontroller only sends
  sentences.
- **Latency.** The flow attends over the whole sentence, so a sentence's z_p must be complete
  before the FPGA starts. The first sound comes after the ESP32's part plus the FPGA's
  compute time for that sentence. After that, the FPGA computes the next sentence while the
  current one plays (it holds two). Splitting text into sentences keeps the first wait short.
- **Fixed point.** The flow and the vocoder run in 16-bit fixed point with 8 to 12-bit
  weights. The gateware is simulated bit for bit against a C++ model of the same integers,
  and that model against TinyTTS in float ([docs/studies.md](docs/studies.md#2-the-vocoder-in-fixed-point),
  [docs/studies.md](docs/studies.md#3-the-flow-in-fixed-point)).

## Documentation

| Document | Contents |
|---|---|
| [Installation](docs/installation.md) | The Arduino library on an ESP32-S3; the development environment: host tools, FPGA toolchains (Gowin EDA, open source), simulators |
| [Getting Started](docs/getting-started.md) | From a PC: bitstream, program upload, speaking, the desktop test |
| [Using the Library](docs/using-the-library.md) | The Arduino API on an ESP32, what runs where, examples, wiring, data headers |
| [Plan and Status](docs/plan-status.md) | The project's phases, the current status, what is open, the plan for the RP2040 |
| [Repository Layout](docs/repository-layout.md) | Directories and the main source files |
| [Studies](docs/studies.md) | The measurements behind the plan, the vocoder and the flow in fixed point (arithmetic, quality against TinyTTS), and the model tool `vocoder_model` |
| [Gateware](docs/gateware.md) | The FPGA design: blocks, the flow unit, hardware image format, link protocol, pins, building, simulation, resources, speed |

## Related projects

- [TinyTTS](https://github.com/pschatzmann/TinyTTS) - the model, the ESP32 code, and
  `docs/tangnano20k.md` with the measurements on this board.
- [arduino-tangnano20k](https://github.com/pschatzmann/arduino-tangnano20k) - the Arduino
  core for the Tang Nano 20K: the open-source FPGA toolchain and openFPGALoader, the INT8
  dot-product accelerator this project replaces for the vocoder, and the verified pins.
- [TangNanoFaust](https://github.com/pschatzmann/NanoTangFaust) - the same SPI and UART pins
  and the PLL settings.
