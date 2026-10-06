/**
 * Text to speech with TinyTTS's front end on an ESP32-S3 and the flow and
 * vocoder on a Tang Nano 20K, linked over SPI. Speaks two sentences in a
 * loop; the FPGA plays them on its onboard I2S amplifier (or its PWM pin).
 *
 * Wiring (Tang Nano 20K header pins, as in TangNanoFaust; 3.3V logic, common GND):
 *   ESP32 SCK  -> FPGA pin 27     ESP32 MOSI -> FPGA pin 28
 *   ESP32 MISO <- FPGA pin 29     ESP32 CS   -> FPGA pin 30
 *
 * The FPGA needs, once from a PC (docs/getting-started.md):
 *   gateware/build.py --gowin --flash        the bitstream, in its flash
 *   tools/tnv.py PORT flash-image            its program image, in its flash
 * It then loads the program at every power-up by itself. (examples/speak_upload
 * uploads it from the sketch instead.)
 *
 * Needs the TinyTTS library, an ESP32-S3 with PSRAM, and Tools > Partition
 * Scheme: Huge APP (3MB No OTA/1MB SPIFFS) - the sketch is about 3.1MB.
 */
#include <SPI.h>

#include "TangNanoVocoderTTS.h"
#include "TangNanoVocoder/data/default_frontend_weights.h"
#include "TinyTTS/data/default_cmudict_slim_data.h"
#include "TinyTTS/data/default_dictionary_model_data.h"

const int kSCK = 12, kMISO = 13, kMOSI = 11, kCS = 10;  // adjust to your board

tnv::SPITransport vocoder_link(SPI, kCS, 4000000);
tnv::TangNanoVocoder vocoder;

void setup() {
  Serial.begin(115200);
  delay(2000);
  SPI.begin(kSCK, kMISO, kMOSI, kCS);

  vocoder.setWeights(default_frontend_weights, default_frontend_weights_len);
  vocoder.setDictionary(default_cmudict_slim, default_cmudict_slim_len);
  vocoder.setDictionaryModel(default_dictionary_model, default_dictionary_model_len);
  if (!vocoder.begin(vocoder_link)) {
    Serial.printf("Vocoder start failed: %s - check wiring, bitstream and program in the FPGA's flash\n",
                  vocoder.error());
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
    Serial.printf("\"%s\": %d frames (%.2fs of audio), z_p made and sent in %lu ms\n", s, vocoder.lastFrames(),
                  vocoder.lastFrames() * 512 / 44100.0f, (unsigned long)(millis() - t0));
  }
  vocoder.waitDone();
  tnv::VocoderStats st;
  if (vocoder.client().stats(st))
    Serial.printf("FPGA: last sentence computed at %.2f x real time\n", st.realTimeFactor(vocoder.lastFrames()));
  delay(5000);
}
