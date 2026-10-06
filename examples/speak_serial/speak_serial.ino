/**
 * Text to speech with TinyTTS's front end on an ESP32-S3 and the flow and
 * vocoder on a Tang Nano 20K, linked over a UART (921600 baud 8N1). Type a
 * sentence into the serial monitor; the FPGA plays it on its onboard I2S
 * amplifier (or its PWM pin).
 *
 * Wiring (Tang Nano 20K header UART, as in TangNanoFaust; 3.3V logic, common GND):
 *   ESP32 TX -> FPGA pin 25 (FPGA RX)     ESP32 RX <- FPGA pin 26 (FPGA TX)
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
#include "TangNanoVocoderTTS.h"
#include "TangNanoVocoder/data/default_frontend_weights.h"
#include "TinyTTS/data/default_cmudict_slim_data.h"
#include "TinyTTS/data/default_dictionary_model_data.h"

const int kRX = 18, kTX = 17;  // ESP32 pins to the FPGA; adjust to your board

tnv::SerialTransport vocoder_link(Serial1);
tnv::TangNanoVocoder vocoder;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial1.begin(921600, SERIAL_8N1, kRX, kTX);

  vocoder.setWeights(default_frontend_weights, default_frontend_weights_len);
  vocoder.setDictionary(default_cmudict_slim, default_cmudict_slim_len);
  vocoder.setDictionaryModel(default_dictionary_model, default_dictionary_model_len);
  if (!vocoder.begin(vocoder_link)) {
    Serial.printf("Vocoder start failed: %s - check wiring, bitstream and program in the FPGA's flash\n",
                  vocoder.error());
    while (true) delay(1000);
  }
  Serial.println("Ready - type a sentence");
}

void loop() {
  if (!Serial.available()) return;
  String text = Serial.readStringUntil('\n');
  text.trim();
  if (text.length() == 0) return;
  if (!vocoder.speak(text.c_str())) Serial.println("speak failed");
  else Serial.printf("sent %d frames\n", vocoder.lastFrames());
}
