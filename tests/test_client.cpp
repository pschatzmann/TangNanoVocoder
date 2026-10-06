// Host tests of the header-only client: the link protocol against a fake
// vocoder that decodes bytes the way gateware/src/vocoder_link.v does, the
// image header, and latent quantization.
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

#include "TangNanoVocoder.h"
#include "TangNanoVocoder/Latent.h"
#include "TangNanoVocoder/data/default_vocoder_image.h"

static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                \
    }                                                            \
  } while (0)

/// The FPGA side of the link, in software: program upload into a word
/// memory, sentences into latent values, '?', 'T', 'R'.
class FakeVocoder : public tnv::VocoderTransport {
 public:
  std::vector<uint32_t> mem = std::vector<uint32_t>(1 << 21, 0);  // the 8MB SDRAM
  std::vector<int16_t> latent;
  int frames = 0, sentences = 0, uploads = 0;
  bool program = false, error = false;

  void write(const uint8_t* data, size_t len) override {
    for (size_t i = 0; i < len; i++) byte(data[i]);
  }
  bool read(uint8_t* data, size_t len, uint32_t) override {
    if (replies.size() < len) return false;
    for (size_t i = 0; i < len; i++) {
      data[i] = replies.front();
      replies.pop_front();
    }
    return true;
  }
  void flushInput() override { replies.clear(); }
  uint32_t millis() override { return clock_ms++; }
  void delay(uint32_t ms) override { clock_ms += ms; }

 private:
  enum State { kIdle, kLen, kData, kSum, kRArg } state = kIdle;
  bool is_prog = false;
  std::vector<uint8_t> buf;
  size_t need = 0;
  uint8_t sum = 0;
  std::deque<uint8_t> replies;
  uint32_t clock_ms = 0;

  uint8_t status() const {
    return 0x80 | (program ? 0x10 | 0x01 : 0) | (error ? 0x08 : 0) | (state != kIdle ? 0x02 : 0);
  }

  void byte(uint8_t b) {
    switch (state) {
      case kIdle:
        if (b == 'P' || b == 'S') {
          is_prog = b == 'P';
          buf.clear();
          need = is_prog ? 4 : 2;
          state = kLen;
        } else if (b == '?') {
          replies.push_back(status());
        } else if (b == 'T') {
          uint8_t r[10] = {status(), 0x40, 0x42, 0x0F, 0x00, 0x20, 0xA1, 0x07, 0x00, 1};  // 1000000, 500000
          replies.insert(replies.end(), r, r + 10);
        } else if (b == 'R') {
          buf.clear();
          need = 6;
          state = kRArg;
        }
        break;
      case kLen:
        buf.push_back(b);
        if (buf.size() == need) {
          uint32_t n = 0;
          for (size_t i = 0; i < need; i++) n |= (uint32_t)buf[i] << (8 * i);
          if (!is_prog) frames = (int)n;
          need = is_prog ? n : n * 64;
          buf.clear();
          sum = 0;
          state = need ? kData : kSum;
        }
        break;
      case kData:
        buf.push_back(b);
        sum += b;
        if (buf.size() == need) state = kSum;
        break;
      case kSum:
        state = kIdle;
        error = b != sum;
        if (error) break;
        if (is_prog) {
          for (size_t i = 0; i + 3 < buf.size(); i += 4)
            mem[i / 4] = buf[i] | buf[i + 1] << 8 | buf[i + 2] << 16 | (uint32_t)buf[i + 3] << 24;
          program = true;
          uploads++;
        } else {
          latent.resize(buf.size() / 2);
          for (size_t i = 0; i < latent.size(); i++) latent[i] = (int16_t)(buf[2 * i] | buf[2 * i + 1] << 8);
          sentences++;
        }
        break;
      case kRArg:
        buf.push_back(b);
        if (buf.size() == need) {
          uint32_t a = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24;
          uint32_t n = buf[4] | buf[5] << 8;
          for (uint32_t i = 0; i < n; i++)
            for (int k = 0; k < 4; k++) replies.push_back((uint8_t)(mem[a + i] >> (8 * k)));
          state = kIdle;
        }
        break;
    }
  }
};

int main() {
  // image header
  tnv::ImageInfo info = tnv::ImageInfo::parse(default_vocoder_image, default_vocoder_image_len);
  CHECK(info.valid);
  CHECK(info.has_flow);  // the default image: flow + vocoder
  CHECK(info.flow_ops == 116);
  CHECK(info.ops == 116 + 102);
  CHECK(info.max_frames == 384);
  CHECK(info.z_scale > 0 && info.z_scale < 0.01f);
  CHECK(!tnv::ImageInfo::parse(default_vocoder_image, 100).valid);

  FakeVocoder fpga;
  tnv::VocoderClient client;
  CHECK(client.begin(fpga));
  CHECK(!client.hasProgram());

  // upload, and the same again only when needed
  size_t last_progress = 0;
  CHECK(client.uploadProgram(default_vocoder_image, default_vocoder_image_len,
                             [&](size_t done, size_t) { last_progress = done; }));
  CHECK(last_progress == default_vocoder_image_len);
  CHECK(fpga.uploads == 1);
  CHECK(std::memcmp(fpga.mem.data(), default_vocoder_image, default_vocoder_image_len) == 0);
  CHECK(client.uploadProgramIfNeeded(default_vocoder_image, default_vocoder_image_len));
  CHECK(fpga.uploads == 1);  // the same program: no upload
  CHECK(client.hasFlow());
  fpga.mem[2] ^= 1;          // the FPGA runs a different program (other header)
  CHECK(client.uploadProgramIfNeeded(default_vocoder_image, default_vocoder_image_len));
  CHECK(fpga.uploads == 2);

  // without an image: the program the FPGA already has (e.g. from its flash)
  tnv::VocoderClient flashed;
  CHECK(flashed.begin(fpga));
  CHECK(flashed.useLoadedProgram());
  CHECK(flashed.zScale() == info.z_scale);
  CHECK(flashed.maxFrames() == 384);
  CHECK(flashed.hasFlow());
  CHECK(fpga.uploads == 2);
  CHECK(client.zScale() == info.z_scale);

  // quantization: round(z / scale), clamped like the model
  tinytts::Mat z(3, 32);
  for (int t = 0; t < 3; t++)
    for (int c = 0; c < 32; c++) z.at(t, c) = (t * 32 + c - 40) * 0.37f * info.z_scale;
  z.at(2, 31) = 1e6f;  // clamps
  std::vector<int16_t> zq = tnv::quantizeLatent(z, info.z_scale);
  CHECK(zq.size() == 96);
  CHECK(zq[0] == (int16_t)std::lround(-40 * 0.37));
  CHECK(zq[95] == 32767);

  // a sentence arrives as sent
  CHECK(client.sendSentence(zq.data(), 3));
  CHECK(fpga.sentences == 1 && fpga.frames == 3);
  CHECK(fpga.latent == zq);
  CHECK(!client.sendSentence(zq.data(), 0));
  CHECK(!client.sendSentence(zq.data(), 449));  // longer than the image allows

  // statistics and readback
  tnv::VocoderStats st;
  CHECK(client.stats(st));
  CHECK(st.run_cycles == 1000000 && st.engine_cycles == 500000 && st.pcm_slot == 1);
  CHECK(std::fabs(st.realTimeFactor(4) - (1000000 / 54e6f) / (4 * 512 / 44100.0f)) < 1e-4f);
  uint32_t words[3];
  CHECK(client.readWords(16, words, 3));
  CHECK(std::memcmp(words, default_vocoder_image + 64, 12) == 0);

  std::printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
