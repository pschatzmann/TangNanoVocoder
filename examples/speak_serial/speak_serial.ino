/**
 * Text to speech with TinyTTS on an ESP32-S3 and the vocoder on a Tang Nano
 * 20K, linked over a UART (921600 baud 8N1). The FPGA plays the audio on its
 * onboard I2S amplifier (or its PWM pin).
 *
 * Wiring (Tang Nano 20K header UART, as in TangNanoFaust; 3.3V logic, common GND):
 *   ESP32 TX -> FPGA pin 25 (FPGA RX)     ESP32 RX <- FPGA pin 26 (FPGA TX)
 * The FPGA runs gateware/build/vocoder.fs (gateware/build.py --flash).
 *
 * Needs the TinyTTS library and a board with PSRAM (see speak_spi).
 */
#include "TangNanoVocoderTTS.h"
#include "TangNanoVocoder/data/default_vocoder_image.h"
#include "TinyTTS/data/default_cmudict_slim_data.h"
#include "TinyTTS/data/default_dictionary_model_data.h"
#include "TinyTTS/data/default_weights_data.h"

const int kRX = 18, kTX = 17;  // ESP32 pins to the FPGA; adjust to your board

tinytts::TinyTTS tts;
tnv::SerialTransport vocoder_link(Serial1);
tnv::TangNanoVocoder vocoder(tts);

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial1.begin(921600, SERIAL_8N1, kRX, kTX);

  tts.setWeights(default_weights, default_weights_len);
  tts.setDictionary(default_cmudict_slim, default_cmudict_slim_len);
  tts.setDictionaryModel(default_dictionary_model, default_dictionary_model_len);
  vocoder.setImage(default_vocoder_image, default_vocoder_image_len);

  if (!vocoder.begin(vocoder_link)) {
    Serial.println("Vocoder not found or upload failed - check wiring and bitstream");
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
