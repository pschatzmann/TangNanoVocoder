/**
 * Text to speech with TinyTTS on an ESP32-S3 and the vocoder on a Tang Nano
 * 20K, linked over SPI. The FPGA plays the audio on its onboard I2S
 * amplifier (or its PWM pin).
 *
 * Wiring (Tang Nano 20K header pins, as in TangNanoFaust; 3.3V logic, common GND):
 *   ESP32 SCK  -> FPGA pin 27     ESP32 MOSI -> FPGA pin 28
 *   ESP32 MISO <- FPGA pin 29     ESP32 CS   -> FPGA pin 30
 * The FPGA runs gateware/build/vocoder.fs (gateware/build.py --flash).
 *
 * Needs the TinyTTS library and a board with PSRAM; the data headers make
 * the sketch large, so use a partition scheme with a big app partition (see
 * TinyTTS's examples/tts_i2s_output, which this follows).
 */
#include <SPI.h>

#include "TangNanoVocoderTTS.h"
#include "TangNanoVocoder/data/default_vocoder_image.h"
#include "TinyTTS/data/default_cmudict_slim_data.h"
#include "TinyTTS/data/default_dictionary_model_data.h"
#include "TinyTTS/data/default_weights_data.h"

const int kSCK = 12, kMISO = 13, kMOSI = 11, kCS = 10;  // adjust to your board

tinytts::TinyTTS tts;
tnv::SPITransport vocoder_link(SPI, kCS, 4000000);
tnv::TangNanoVocoder vocoder(tts);

void setup() {
  Serial.begin(115200);
  delay(2000);
  SPI.begin(kSCK, kMISO, kMOSI, kCS);

  tts.setWeights(default_weights, default_weights_len);
  tts.setDictionary(default_cmudict_slim, default_cmudict_slim_len);
  tts.setDictionaryModel(default_dictionary_model, default_dictionary_model_len);
  vocoder.setImage(default_vocoder_image, default_vocoder_image_len);

  Serial.println("Starting: TinyTTS, then the FPGA (program upload if needed)...");
  if (!vocoder.begin(vocoder_link, [](size_t done, size_t total) {
        if (done == total || done % (64 * 1024) < 1024) Serial.printf("  upload %u%%\n", (unsigned)(100 * done / total));
      })) {
    Serial.println("Vocoder not found or upload failed - check wiring and bitstream");
    while (true) delay(1000);
  }
  Serial.println("Ready");
}

void loop() {
  const char* sentences[] = {"Hello world!", "This is the Tang Nano vocoder speaking."};
  for (const char* s : sentences) {
    uint32_t t0 = millis();
    if (!vocoder.speak(s)) {
      Serial.println("speak failed");
      continue;
    }
    Serial.printf("\"%s\": %d frames (%.2fs of audio), latent made and sent in %lu ms\n", s, vocoder.lastFrames(),
                  vocoder.lastFrames() * 512 / 44100.0f, (unsigned long)(millis() - t0));
  }
  vocoder.waitDone();
  tnv::VocoderStats st;
  if (vocoder.client().stats(st))
    Serial.printf("FPGA: last sentence computed at %.2f x real time\n", st.realTimeFactor(vocoder.lastFrames()));
  delay(5000);
}
