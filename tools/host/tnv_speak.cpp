// tnv_speak: text -> TinyTTS on the PC -> the Tang Nano 20K vocoder over its
// USB serial port, with the same header-only client an ESP32 uses
// (src/TangNanoVocoder). The board plays the audio over I2S.
//
//   tnv_speak /dev/ttyUSB1 "Hello world!" ["Another sentence." ...]
//
// Uploads the program image first if the board has none. Prints each
// sentence's length and the board's compute time ('T').
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "TangNanoVocoder.h"
#include "TangNanoVocoder/Latent.h"
#include "TangNanoVocoder/PosixSerialTransport.h"
#include "TangNanoVocoder/data/default_vocoder_image.h"
#include "TinyTTS/TinyTTSCore.h"

static std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    std::exit(1);
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: tnv_speak PORT TEXT [TEXT ...]\n");
    return 1;
  }
  tnv::PosixSerialTransport port;
  if (!port.open(argv[1])) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  tnv::VocoderClient vocoder;
  if (!vocoder.begin(port)) {
    std::fprintf(stderr, "no answer from the vocoder on %s - bitstream loaded?\n", argv[1]);
    return 1;
  }
  bool had = vocoder.hasProgram();
  if (!vocoder.uploadProgramIfNeeded(default_vocoder_image, default_vocoder_image_len,
                                     [](size_t done, size_t total) {
                                       std::fprintf(stderr, "\rupload %3u%%", (unsigned)(100 * done / total));
                                     })) {
    std::fprintf(stderr, "\nprogram upload failed\n");
    return 1;
  }
  if (!had) std::fprintf(stderr, "\n");

  // TinyTTS on the PC (an ESP32 does the same with its flash data)
  std::string research = std::string(TNV_TINYTTS_DIR) + "/research/";
  static std::vector<uint8_t> weights = readFile(research + "weights.bin");
  static std::vector<uint8_t> cmudict = readFile(research + "cmudict.bin");
  static std::vector<uint8_t> dict_model = readFile(research + "dictionary_model.bin");
  static tinytts::TinyTTSCore core;
  if (!core.begin(weights.data(), weights.size(), cmudict.data(), cmudict.size(), 2, 4, 4, dict_model.data(),
                  dict_model.size())) {
    std::fprintf(stderr, "TinyTTS failed to load %s\n", research.c_str());
    return 1;
  }

  int frames = 0;
  for (int i = 2; i < argc; i++) {
    tnv::Latent latent = tnv::latentFromText(core, argv[i]);
    frames = latent.z.rows();
    std::vector<int16_t> zq = tnv::quantizeLatent(latent.z, vocoder.zScale());
    if (!vocoder.sendSentence(zq.data(), frames)) {
      std::fprintf(stderr, "sending \"%s\" failed\n", argv[i]);
      return 1;
    }
    std::printf("\"%s\": %d frames, %.2fs of audio, sent\n", argv[i], frames, frames * 512 / 44100.0);
  }
  if (!vocoder.waitDone()) {
    std::fprintf(stderr, "the vocoder did not finish\n");
    return 1;
  }
  tnv::VocoderStats st;
  if (vocoder.stats(st))
    std::printf("board: last sentence computed in %.3fs = %.2f x real time\n", st.run_cycles / 54e6,
                st.realTimeFactor(frames));
  return 0;
}
