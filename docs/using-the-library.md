# Using the Library

This repository is a header-only Arduino library (`src/`). The sketch below assumes the setup
from [installation.md](installation.md):
- an ESP32-S3 with the TinyTTS library installed;
- a Tang Nano 20K with the bitstream and its program image in its flash.

```c++
#include "TangNanoVocoderTTS.h"
#include "TangNanoVocoder/data/default_frontend_weights.h"   // TinyTTS's front end, 0.66MB
#include "TinyTTS/data/default_cmudict_slim_data.h"
#include "TinyTTS/data/default_dictionary_model_data.h"

tnv::SerialTransport vocoder_link(Serial1);                  // or tnv::SPITransport(SPI, cs)
tnv::TangNanoVocoder vocoder;

void setup() {
  Serial1.begin(921600, SERIAL_8N1, 18, 17);                 // RX, TX to FPGA pins 26, 25
  vocoder.setWeights(default_frontend_weights, default_frontend_weights_len);
  vocoder.setDictionary(default_cmudict_slim, default_cmudict_slim_len);
  vocoder.setDictionaryModel(default_dictionary_model, default_dictionary_model_len);
  if (!vocoder.begin(vocoder_link)) Serial.println(vocoder.error());   // finds the FPGA and its program
  vocoder.speak("Hello world!");                             // the FPGA computes and plays it
}
```

## What runs where

The ESP32 runs TinyTTS's **front end**: G2P, text encoder and duration predictor
(`tnv::FrontEnd`). It turns text into z_p, the flow's input. The FPGA runs the **flow and
the vocoder**, and plays the audio over I2S or its PWM pin. This split pays off on the
ESP32:

- **Flash:** the ESP32 needs only the front end's weights, 0.66MB of TinyTTS's 2.07MB.
- **Memory:** no flow, so no memory for its attention over the whole sentence.
- **Time:** about 0.15 x real time on an ESP32-S3 (an estimate, not measured on an ESP32
  yet). With the flow it was 2.5 x (3.7s for 1.5s of audio, measured).

## Sentences

- `speak()` returns once the sentence is sent. The FPGA computes and plays it while the
  next one is prepared, and it holds two.
- One sentence may have at most 384 latent frames, about 4.5s of speech (`maxFrames()`).
  `speak()` returns false for a longer one, so split long texts into sentences. A latent
  frame is 512 samples at 44.1kHz (11.6ms).
- The FPGA computes a sentence at about 0.9 x real time. The first sound comes after that,
  plus the ESP32's part.
- `setNoiseScale()`, `setLengthScale()` and `setSeed()` work as in TinyTTS.
- `waitDone()` waits until everything sent has been computed and played.
  `client().stats()` gives the board's cycle count for the last sentence
  (`realTimeFactor()`).
- **Speaker:** TinyTTS's model has one speaker, so there is no speaker setting.

## Program image

The FPGA runs a program image: the flow's and the vocoder's ops and weights, 1.6MB. It can
get it in two ways.

**From its own flash (the default).** Write it there once from a PC:

```bash
tools/tnv.py /dev/ttyUSB1 flash-image
```

From then on, the FPGA loads it at every power-up, in about 1s, and the sketch doesn't
carry it. `begin()` waits for the FPGA to answer, which it doesn't while it loads. Then it
reads the program's header back from the FPGA: the latent scale, the longest sentence, and
whether the program has the flow.

**From the sketch** (`examples/speak_upload`). Include `default_vocoder_image.h` and call
`setImage()`. `begin()` then compares the image's header with the FPGA's program, and
uploads the image only if they differ. The upload is needed again after every power-up of
the FPGA, unless the same image is in its flash:
- about 17s over a UART at 921600 baud;
- about 4s over SPI at 4 MHz.

`begin()`'s optional progress callback reports the upload.

If `begin()` fails, `error()` says why:
- the front-end data is missing;
- the image is invalid, or has no flow;
- the FPGA doesn't answer;
- the FPGA has no program and no image was set;
- the upload failed.

## Lower level

`TangNanoVocoder.h` on its own is the link without TinyTTS: `tnv::VocoderClient` handles
status, program upload, sentences (quantized latents), statistics and SDRAM readback. It
works over `SPITransport`, `SerialTransport` or your own `VocoderTransport`. With a
vocoder-only program image (`tools/make_image_header.py --vocoder-only`), the MCU has to
run the flow itself:
- use `latentFromText()` with all of TinyTTS's weights;
- send z.

## Examples and wiring

The examples have the wiring in their headers:

| Example | Link | What it does | Program image | Sketch | Partition scheme |
|---|---|---|---|---|---|
| `speak_spi` | SPI | speaks fixed sentences in a loop | in the FPGA's flash | 3.1MB | Huge APP (3MB No OTA/1MB SPIFFS) |
| `speak_serial` | UART | speaks what you type into the serial monitor | in the FPGA's flash | 3.1MB | Huge APP (3MB No OTA/1MB SPIFFS) |
| `speak_upload` | UART | like `speak_serial` | in the sketch, uploaded when needed | 4.7MB | Custom: the `partitions.csv` next to it (6MB app) |

The FPGA's pins are the same as TangNanoFaust's (3.3V logic, common GND):

| Link | FPGA pins |
|---|---|
| SPI | 27 (SCLK), 28 (MOSI), 29 (MISO), 30 (CS) |
| Header UART | 25 (FPGA RX), 26 (FPGA TX) |

All of them need an ESP32-S3 with PSRAM:
- `speak_spi` and `speak_serial`: 4MB of flash or more. They use 99% of Huge APP's 3MB app
  partition, so there is little room for more code in them.
- `speak_upload`: 8MB of flash or more.

Almost all of a sketch is data:

| Data | Size |
|---|---|
| FPGA program image | 1.59MB |
| Slim CMU dictionary | 1.09MB |
| G2P model for unknown words | 0.97MB |
| Front-end weights | 0.65MB |

- **Program image:** only in `speak_upload`; the others leave it in the FPGA's flash.
- **Other data:** it can also come from files on LittleFS or SD. The `set...(File&)`
  overloads load them into PSRAM.

## CMake and ESP-IDF

The top-level `CMakeLists.txt` provides the INTERFACE library
`TangNanoVocoder::TangNanoVocoder` and registers as an ESP-IDF component. On request it also
builds host targets:
- tests;
- `tnv_speak`;
- `vocoder_model`.

See [installation.md](installation.md#development-environment).

## Regenerating the data headers

| Command | Generates |
|---|---|
| `tools/make_image_header.py` | `default_vocoder_image.h`, after model changes (`--vocoder-only` for the vocoder alone). After a change, also write it to the FPGA's flash again (`tnv.py flash-image`) |
| `tools/make_frontend_weights.py` | `default_frontend_weights.h`, from TinyTTS's `research/weights.bin` |
