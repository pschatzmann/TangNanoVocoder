/**
 * Like speak_serial, but the sketch carries the FPGA's program image (1.6MB)
 * and uploads it when the FPGA doesn't run it yet - for a board whose flash
 * holds only the bitstream (no tools/tnv.py flash-image), or to try a new
 * image without flashing it. The upload takes about 17s over the UART (about
 * 4s over SPI) and is needed again after every power-up of the FPGA; begin()
 * skips it when the FPGA already runs this program. Type a sentence into the
 * serial monitor; the FPGA plays it on its onboard I2S amplifier.
 *
 * Wiring (Tang Nano 20K header UART, as in TangNanoFaust; 3.3V logic, common GND):
 *   ESP32 TX -> FPGA pin 25 (FPGA RX)     ESP32 RX <- FPGA pin 26 (FPGA TX)
 * The FPGA's bitstream, once from a PC: gateware/build.py --gowin --flash
 *
 * Needs the TinyTTS library and an ESP32-S3 with PSRAM and 8MB flash or more.
 * The sketch is about 4.7MB: use the partition table next to it (Tools >
 * Partition Scheme: Custom), a 6MB app partition.
 */
#include "TangNanoVocoderTTS.h"
#include "TangNanoVocoder/data/default_frontend_weights.h"
#include "TangNanoVocoder/data/default_vocoder_image.h"
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
  vocoder.setImage(default_vocoder_image, default_vocoder_image_len);

  Serial.println("Starting: front end, then the FPGA (program upload if needed)...");
  bool ok = vocoder.begin(vocoder_link, [](size_t done, size_t total) {
    if (done == total || done % (64 * 1024) < 1024) Serial.printf("  upload %u%%\n", (unsigned)(100 * done / total));
  });
  if (!ok) {
    Serial.printf("Vocoder start failed: %s - check wiring and bitstream\n", vocoder.error());
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
