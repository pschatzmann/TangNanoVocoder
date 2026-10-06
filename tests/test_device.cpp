// Hardware-in-the-loop test: sentences to a Tang Nano 20K running the
// vocoder bitstream, over its USB serial port, with the library's client.
// The board computes each sentence and plays it over I2S (onboard amplifier
// - listen). The test checks the protocol, waits for playback, reports the
// board's speed, and compares part of the last sentence's PCM, read back
// from SDRAM, with the fixed-point model's (vocoder_model --pcm-out).
//
//   test_device PORT [MODEL]      e.g. test_device /dev/ttyUSB1 build/model/vocoder_model
//
// Registered with CTest when configured with -DTNV_DEVICE_PORT=/dev/ttyUSB1.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "TangNanoVocoder.h"
#include "TangNanoVocoder/Latent.h"
#include "TangNanoVocoder/PosixSerialTransport.h"
#include "TangNanoVocoder/data/default_vocoder_image.h"
#include "TinyTTS/TinyTTSCore.h"

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

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: test_device PORT [MODEL]\n");
    return 2;
  }
  const char* sentences[] = {
      "Hello world!",
      "This is the Tang Nano vocoder speaking.",
      "The quick brown fox jumps over the lazy dog.",
  };

  tnv::PosixSerialTransport port;
  if (!port.open(argv[1])) {
    std::printf("FAIL: cannot open %s\n", argv[1]);
    return 1;
  }
  tnv::VocoderClient vocoder;
  if (!vocoder.begin(port)) {
    std::printf("FAIL: no answer on %s - is the bitstream loaded (gateware/build.py --load)?\n", argv[1]);
    return 1;
  }
  CHECK(vocoder.uploadProgramIfNeeded(default_vocoder_image, default_vocoder_image_len,
                                      [](size_t done, size_t total) {
                                        std::fprintf(stderr, "\rupload %3u%%", (unsigned)(100 * done / total));
                                      }));
  std::fprintf(stderr, "\n");
  CHECK(vocoder.hasProgram());
  CHECK(vocoder.waitDone(10000));  // nothing left over from before

  std::string research = std::string(TNV_TINYTTS_DIR) + "/research/";
  static std::vector<uint8_t> weights = readFile(research + "weights.bin");
  static std::vector<uint8_t> cmudict = readFile(research + "cmudict.bin");
  static std::vector<uint8_t> dict_model = readFile(research + "dictionary_model.bin");
  static tinytts::TinyTTSCore core;
  if (!core.begin(weights.data(), weights.size(), cmudict.data(), cmudict.size(), 2, 4, 4, dict_model.data(),
                  dict_model.size())) {
    std::printf("FAIL: TinyTTS data not found in %s\n", research.c_str());
    return 1;
  }

  // send all sentences: two latent slots, so sendSentence() waits while both are full
  int frames = 0;
  double audio_s = 0;
  for (const char* text : sentences) {
    // noise seed 200: what vocoder_model uses for its (first) sentence, so the
    // last one can be compared with the model's PCM
    // a program with the flow takes z_p (the FPGA runs the flow), else z
    tnv::Latent latent = vocoder.hasFlow() ? tnv::latentPrior(core, text, 0, 0.667f, 1.0f, 200)
                                           : tnv::latentFromText(core, text, 0, 0.667f, 1.0f, 200);
    frames = latent.z.rows();
    std::vector<int16_t> zq = tnv::quantizeLatent(latent.z, vocoder.zScale());
    bool sent = vocoder.sendSentence(zq.data(), frames);
    CHECK(sent);
    audio_s += frames * 512 / 44100.0;
    std::printf("sent \"%s\": %d frames, %.2fs of audio\n", text, frames, frames * 512 / 44100.0);
  }
  CHECK(vocoder.waitDone((uint32_t)(30000 + 3000 * audio_s)));
  int st_byte = vocoder.status();
  CHECK(st_byte >= 0 && !(st_byte & tnv::kError));

  tnv::VocoderStats st;
  CHECK(vocoder.stats(st));
  float rtf = st.realTimeFactor(frames);
  std::printf("board: last sentence computed in %.3fs = %.2f x real time (engine busy %.0f%%)\n",
              st.run_cycles / 54e6, rtf, 100.0 * st.engine_cycles / (st.run_cycles ? st.run_cycles : 1));
  CHECK(st.run_cycles > 0 && rtf < 2.0f);

  // the last sentence's PCM against the model's (first 2048 samples)
  if (argc > 2) {
    std::string pcm_path = "test_device_pcm.bin";
    std::string cmd = std::string(argv[2]) + (vocoder.hasFlow() ? " --flow --flow-wbits 12 --flow-headroom 1.25" : "") +
                      " --text \"" + sentences[2] + "\" --pcm-out " + pcm_path +
                      " > /dev/null 2>&1";
    if (std::system(cmd.c_str()) == 0) {
      std::vector<uint8_t> want = readFile(pcm_path);
      const size_t words = 1024;
      std::vector<uint32_t> got(words);
      CHECK(want.size() >= words * 4);
      CHECK(vocoder.readWords(vocoder.imageInfo().pcm_base[st.pcm_slot], got.data(), words));
      size_t bad = 0;
      for (size_t i = 0; i < words * 2 && i * 2 + 1 < want.size(); i++) {
        uint16_t g = (uint16_t)(got[i / 2] >> (16 * (i & 1)));
        uint16_t w = (uint16_t)(want[2 * i] | want[2 * i + 1] << 8);
        bad += g != w;
      }
      std::printf("PCM: %zu of %zu samples differ from the model\n", bad, words * 2);
      CHECK(bad == 0);
    } else {
      std::printf("model not runnable (%s) - PCM not compared\n", argv[2]);
    }
  }

  std::printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
