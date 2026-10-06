// The Arduino library on the desktop: exactly what examples/speak_serial does
// on an ESP32 - tnv::TangNanoVocoder with the library's data headers (front
// end weights, TinyTTS's slim dictionary and G2P model, the FPGA program
// image) - with the PC's serial port to the Tang Nano 20K in place of
// Serial1. The FPGA runs the flow and the vocoder and plays the sentences
// over I2S (onboard amplifier - listen).
//
// Checks: the program upload (only if the board runs a different program),
// the sentences, the board's speed; with the model given, the last
// sentence's z_p as sent against the model's link packet, and its PCM read
// back from SDRAM against the model's, sample by sample.
//
//   test_desktop PORT [MODEL] [--no-image]
//     e.g. test_desktop /dev/ttyUSB1 build/model/vocoder_model
//   --no-image: no program image in the "sketch" - the FPGA must have loaded
//   it from its flash (tools/tnv.py flash-image)
//
// Registered with CTest (label device) when configured with
// -DTNV_DEVICE_PORT=/dev/ttyUSB1.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "TangNanoVocoderTTS.h"
#include "TangNanoVocoder/PosixSerialTransport.h"
#include "TangNanoVocoder/data/default_frontend_weights.h"
#include "TangNanoVocoder/data/default_vocoder_image.h"
#include "TinyTTS/data/default_cmudict_slim_data.h"
#include "TinyTTS/data/default_dictionary_model_data.h"

static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                \
    }                                                            \
  } while (0)

static std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// the vocoder_model options of the flow image (tools/make_image_header.py)
static const char* kFlowArgs = " --flow --flow-wbits 12 --flow-headroom 1.25";

int main(int argc, char** argv) {
  bool no_image = false;
  std::vector<char*> args;
  for (int i = 0; i < argc; i++) {
    if (std::string(argv[i]) == "--no-image") no_image = true;
    else args.push_back(argv[i]);
  }
  argc = (int)args.size();
  argv = args.data();
  if (argc < 2) {
    std::printf("usage: test_desktop PORT [MODEL] [--no-image]\n");
    return 2;
  }
  const char* sentences[] = {
      "Hello world!",
      "This is the Tang Nano vocoder speaking.",
      "The quick brown fox jumps over the lazy dog.",
  };
  const uint32_t kSeed = 200;  // vocoder_model's noise seed for its sentence

  // ---- as in setup() of examples/speak_serial
  tnv::PosixSerialTransport vocoder_link;  // Serial1 on the ESP32
  if (!vocoder_link.open(argv[1])) {
    std::printf("FAIL: cannot open %s\n", argv[1]);
    return 1;
  }
  static tnv::TangNanoVocoder vocoder;
  vocoder.setWeights(default_frontend_weights, default_frontend_weights_len);
  vocoder.setDictionary(tinytts::default_cmudict_slim, tinytts::default_cmudict_slim_len);
  vocoder.setDictionaryModel(tinytts::default_dictionary_model, tinytts::default_dictionary_model_len);
  if (!no_image) vocoder.setImage(default_vocoder_image, default_vocoder_image_len);
  vocoder.setSeed(kSeed);
  double t0 = now();
  bool started = vocoder.begin(vocoder_link, [](size_t done, size_t total) {
    std::fprintf(stderr, "\rupload %3u%%", (unsigned)(100 * done / total));
  });
  std::fprintf(stderr, "\n");
  if (!started) {
    std::printf("FAIL: begin: %s\n", vocoder.error());
    return 1;
  }
  std::printf("begin: %.1fs (front end, program %s), program with the flow: %s, max %d frames\n", now() - t0,
              no_image ? "from the FPGA's flash" : "check/upload", vocoder.client().hasFlow() ? "yes" : "no",
              vocoder.maxFrames());
  if (no_image) {  // the program the FPGA loaded must be this library's image
    tnv::ImageInfo lib = tnv::ImageInfo::parse(default_vocoder_image, default_vocoder_image_len);
    bool same = std::memcmp(lib.header, vocoder.client().imageInfo().header, sizeof(lib.header)) == 0;
    std::printf("the FPGA's program %s the library's image\n", same ? "is" : "is NOT");
    CHECK(same);
  }
  CHECK(vocoder.client().hasFlow());
  CHECK(vocoder.waitDone(10000));  // nothing left over from before

  // ---- as in loop(): speak, then wait for the playback
  double audio_s = 0;
  for (const char* text : sentences) {
    double t = now();
    bool sent = vocoder.speak(text);
    CHECK(sent);
    double s = vocoder.lastFrames() * 512 / 44100.0;
    audio_s += s;
    std::printf("speak \"%s\": %d frames, %.2fs of audio; z_p made and sent in %.2fs\n", text,
                vocoder.lastFrames(), s, now() - t);
  }
  CHECK(vocoder.waitDone((uint32_t)(30000 + 3000 * audio_s)));
  int status = vocoder.client().status();
  CHECK(status >= 0 && !(status & tnv::kError));

  tnv::VocoderStats st;
  CHECK(vocoder.client().stats(st));
  float rtf = st.realTimeFactor(vocoder.lastFrames());
  std::printf("FPGA: last sentence (flow + vocoder) computed in %.3fs = %.2f x real time (engine busy %.0f%%)\n",
              st.run_cycles / 54e6, rtf, 100.0 * st.engine_cycles / (st.run_cycles ? st.run_cycles : 1));
  CHECK(st.run_cycles > 0 && rtf < 2.0f);

  // ---- the last sentence against the fixed-point model
  if (argc > 2) {
    const char* text = sentences[2];
    std::string packet_path = "test_desktop_zp.bin", pcm_path = "test_desktop_pcm.bin";
    std::string cmd = std::string(argv[2]) + kFlowArgs + " --text \"" + text + "\" --sentence-out " + packet_path +
                      " --pcm-out " + pcm_path + " > /dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) {
      std::printf("FAIL: %s\n", cmd.c_str());
      failures++;
    } else {
      // z_p: the library's against the model's link packet ('S', u16 frames, values, checksum)
      tnv::Latent zp = vocoder.frontEnd().latent(text, 0, 0.667f, 1.0f, kSeed);
      std::vector<int16_t> zq = tnv::quantizeLatent(zp.z, vocoder.client().zScale());
      std::vector<uint8_t> packet = readFile(packet_path);
      int frames = packet.size() >= 3 ? packet[1] | packet[2] << 8 : 0;
      size_t zbad = 0;
      CHECK(frames == zp.z.rows());
      for (size_t i = 0; i < zq.size() && 3 + 2 * i + 1 < packet.size(); i++)
        zbad += zq[i] != (int16_t)(packet[3 + 2 * i] | packet[4 + 2 * i] << 8);
      std::printf("z_p: %d frames, %zu of %zu values differ from the model's\n", frames, zbad, zq.size());
      CHECK(zbad == 0);

      // PCM read back from the board
      std::vector<uint8_t> want = readFile(pcm_path);
      size_t samples = (size_t)vocoder.lastFrames() * 512, words = samples / 2;
      CHECK(want.size() == samples * 2);
      std::vector<uint32_t> got(words);
      double tr = now();
      CHECK(vocoder.client().readWords(vocoder.client().imageInfo().pcm_base[st.pcm_slot], got.data(), words, 4096));
      size_t bad = 0;
      for (size_t i = 0; i < samples && 2 * i + 1 < want.size(); i++) {
        uint16_t g = (uint16_t)(got[i / 2] >> (16 * (i & 1)));
        uint16_t w = (uint16_t)(want[2 * i] | want[2 * i + 1] << 8);
        bad += g != w;
      }
      std::printf("PCM: %zu of %zu samples differ from the model%s (read back in %.1fs)\n", bad, samples,
                  bad == 0 ? " - bit-exact" : "", now() - tr);
      CHECK(bad == 0);
    }
  }

  std::printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
