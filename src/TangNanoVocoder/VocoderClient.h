#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

namespace tnv {

/// Status byte flags (docs/gateware.md, "Link"). A valid status has bit 7
/// set and bits 6..5 clear.
enum StatusFlags : uint8_t {
  kReady = 1,       ///< a sentence may be sent
  kReceiving = 2,   ///< inside a packet
  kBusy = 4,        ///< computing or playing
  kError = 8,       ///< last packet dropped
  kProgram = 16,    ///< a program image is loaded
};

/// The hardware image's header (vocoder_model --export-hw): what the host
/// needs to know about it.
struct ImageInfo {
  bool valid = false;
  uint32_t ops = 0;
  uint32_t max_frames = 0;   ///< longest sentence, in latent frames
  uint32_t pcm_base[2] = {0, 0};
  uint32_t words = 0;        ///< image size in 32-bit words
  float z_scale = 0;         ///< latent quantization: value = round(z / z_scale)
  bool has_flow = false;     ///< the FPGA runs the flow too: sentences are z_p, not z
  uint32_t flow_ops = 0;     ///< of ops, the flow's (they come first)
  uint32_t header[16] = {};  ///< the raw header words

  static ImageInfo parse(const uint8_t* image, size_t len) {
    ImageInfo info;
    if (image == nullptr || len < 64) return info;
    uint32_t h[16];
    std::memcpy(h, image, sizeof(h));  // little endian, as the image is
    info = fromHeader(h);
    info.valid = info.valid && info.words * 4 == len;
    return info;
  }

  /// From the 16 header words alone (e.g. read back from the FPGA's SDRAM).
  static ImageInfo fromHeader(const uint32_t* h) {
    ImageInfo info;
    if (h[0] != 0x48564E54u || h[1] != 1) return info;
    info.ops = h[2];
    info.max_frames = h[4];
    info.pcm_base[0] = h[8];
    info.pcm_base[1] = h[9];
    info.words = h[10];
    std::memcpy(&info.z_scale, &h[11], 4);
    info.has_flow = (h[13] & 1) != 0;
    info.flow_ops = info.has_flow ? h[14] : 0;
    std::memcpy(info.header, h, sizeof(info.header));
    info.valid = info.words >= 16;
    return info;
  }
};

/// Statistics of the last sentence ('T').
struct VocoderStats {
  uint32_t run_cycles = 0;     ///< from start to PCM written
  uint32_t engine_cycles = 0;  ///< of those, with the convolution engine busy
  uint8_t pcm_slot = 0;
  /// compute time / audio time at the given clock, for `frames` latent frames
  float realTimeFactor(int frames, float clock_hz = 54e6f) const {
    return frames > 0 ? (run_cycles / clock_hz) / (frames * 512 / 44100.0f) : 0;
  }
};

/**
 * @brief Byte transport to the vocoder: SPI or a UART (Transports.h), or
 * anything else that moves bytes. read() must time out.
 */
class VocoderTransport {
 public:
  virtual ~VocoderTransport() = default;
  virtual void write(const uint8_t* data, size_t len) = 0;
  /// Reads exactly `len` reply bytes; false on timeout.
  virtual bool read(uint8_t* data, size_t len, uint32_t timeout_ms) = 0;
  /// Discards pending input (UART); nothing for SPI.
  virtual void flushInput() {}
  /// Millisecond clock and delay (Arduino millis()/delay() in the transports).
  virtual uint32_t millis() = 0;
  virtual void delay(uint32_t ms) = 0;
};

/**
 * @brief The link protocol to the Tang Nano 20K vocoder (gateware/src/
 * vocoder_link.v): upload the program image, send sentences (quantized
 * latents), read status, statistics and SDRAM. Transport independent and
 * header only.
 *
 *   tnv::VocoderClient vocoder;
 *   vocoder.begin(transport);
 *   vocoder.uploadProgramIfNeeded(default_vocoder_image, default_vocoder_image_len);
 *   vocoder.sendSentence(zq.data(), frames);   // the FPGA computes and plays it
 */
class VocoderClient {
 public:
  using ProgressFn = std::function<void(size_t done, size_t total)>;

  /// Finds the vocoder. Waits up to `timeout_ms` for an answer: after
  /// power-up the FPGA loads its program from flash first (about 1s) and
  /// doesn't answer meanwhile.
  bool begin(VocoderTransport& transport, uint32_t timeout_ms = 3000) {
    t_ = &transport;
    uint32_t start = t_->millis();
    while (true) {
      int s = status();
      if (s >= 0 && !(s & kReceiving)) return true;
      if (t_->millis() - start > timeout_ms) return s >= 0;
      t_->delay(20);
    }
  }

  /// Uses the program the FPGA already runs (loaded from its flash at
  /// power-up, or uploaded before): reads its header back from SDRAM, for
  /// zScale(), maxFrames() and hasFlow(). False if it has none.
  bool useLoadedProgram() {
    if (!hasProgram()) return false;
    uint32_t h[16];
    if (!readWords(0, h, 16)) return false;
    info_ = ImageInfo::fromHeader(h);
    return info_.valid;
  }

  /// The status byte, or -1 when nothing answers.
  int status() {
    t_->flushInput();
    const uint8_t q = '?';
    t_->write(&q, 1);
    uint8_t r = 0;
    if (!t_->read(&r, 1, 200) || (r & 0xE0) != 0x80) return -1;
    return r;
  }

  bool isReady() {
    int s = status();
    return s >= 0 && (s & kReady);
  }

  bool isBusy() {
    int s = status();
    return s >= 0 && (s & kBusy);
  }

  bool hasProgram() {
    int s = status();
    return s >= 0 && (s & kProgram);
  }

  /// Polls until (status & mask) == value; false on timeout or no answer.
  bool waitStatus(uint8_t mask, uint8_t value, uint32_t timeout_ms) {
    uint32_t start = t_->millis();
    while (true) {
      int s = status();
      if (s >= 0 && (s & mask) == value) return true;
      if (t_->millis() - start > timeout_ms) return false;
      t_->delay(5);
    }
  }

  /// Uploads the program image ('P'). Only while the vocoder is idle.
  bool uploadProgram(const uint8_t* image, size_t len, ProgressFn progress = nullptr) {
    info_ = ImageInfo::parse(image, len);
    if (!info_.valid) return false;
    if (!waitStatus(kBusy | kReceiving, 0, 60000)) return false;
    uint8_t head[5] = {'P', (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
    t_->write(head, 5);
    uint8_t sum = 0;
    const size_t chunk = 1024;
    uint8_t buf[chunk];
    for (size_t pos = 0; pos < len; pos += chunk) {
      size_t n = len - pos < chunk ? len - pos : chunk;
      std::memcpy(buf, image + pos, n);  // the image may be in flash
      for (size_t i = 0; i < n; i++) sum += buf[i];
      t_->write(buf, n);
      if (progress) progress(pos + n, len);
    }
    t_->write(&sum, 1);
    if (!waitStatus(kReceiving, 0, 5000)) return false;
    return hasProgram();
  }

  /// Uploads the image unless the vocoder already has this program: one
  /// with the same header (read back from SDRAM word 0). The image is
  /// parsed either way, for zScale(), maxFrames() and hasFlow().
  bool uploadProgramIfNeeded(const uint8_t* image, size_t len, ProgressFn progress = nullptr) {
    info_ = ImageInfo::parse(image, len);
    if (!info_.valid) return false;
    if (hasProgram() && !isBusy()) {
      uint32_t loaded[16];
      if (readWords(0, loaded, 16) && std::memcmp(loaded, info_.header, sizeof(loaded)) == 0) return true;
    } else if (hasProgram()) {
      return true;  // busy: it plays what was sent for this program
    }
    return uploadProgram(image, len, progress);
  }

  /// Sends one sentence ('S'): `frames` x 32 int16, frame by frame, already
  /// quantized (quantizeLatent()). Waits until the vocoder is ready for it
  /// (up to timeout_ms); it then computes and plays it on its own.
  bool sendSentence(const int16_t* zq, int frames, uint32_t timeout_ms = 60000) {
    if (frames <= 0 || frames > 65535 || (info_.valid && (uint32_t)frames > info_.max_frames)) return false;
    if (!waitStatus(kReady, kReady, timeout_ms)) return false;
    uint8_t head[3] = {'S', (uint8_t)frames, (uint8_t)(frames >> 8)};
    t_->write(head, 3);
    uint8_t sum = 0;
    uint8_t buf[256];
    size_t values = (size_t)frames * 32, pos = 0;
    while (pos < values) {
      size_t n = 0;
      for (; n < sizeof(buf) / 2 && pos < values; n++, pos++) {
        uint16_t v = (uint16_t)zq[pos];
        buf[2 * n] = (uint8_t)v;
        buf[2 * n + 1] = (uint8_t)(v >> 8);
        sum += buf[2 * n] + buf[2 * n + 1];
      }
      t_->write(buf, 2 * n);
    }
    t_->write(&sum, 1);
    // accepted once the packet is over without an error
    if (!waitStatus(kReceiving, 0, 5000)) return false;
    int s = status();
    return s >= 0 && !(s & kError);
  }

  /// Waits until everything sent has been computed and played.
  bool waitDone(uint32_t timeout_ms = 120000) { return waitStatus(kBusy, 0, timeout_ms); }

  /// Statistics of the last sentence ('T').
  bool stats(VocoderStats& out) {
    t_->flushInput();
    const uint8_t q = 'T';
    t_->write(&q, 1);
    uint8_t r[10];
    if (!t_->read(r, 10, 500)) return false;
    out.run_cycles = r[1] | r[2] << 8 | r[3] << 16 | (uint32_t)r[4] << 24;
    out.engine_cycles = r[5] | r[6] << 8 | r[7] << 16 | (uint32_t)r[8] << 24;
    out.pcm_slot = r[9];
    return true;
  }

  /// Reads SDRAM words ('R'), e.g. the last sentence's PCM at
  /// imageInfo().pcm_base[stats.pcm_slot]. `burst` words per request: 1 is
  /// safe over SPI at any clock; over a UART up to 65535 work (faster).
  bool readWords(uint32_t addr, uint32_t* out, size_t count, size_t burst = 1) {
    if (burst < 1) burst = 1;
    if (burst > 65535) burst = 65535;
    uint8_t r[4 * 64];
    for (size_t i = 0; i < count;) {
      size_t n = count - i < burst ? count - i : burst;
      t_->flushInput();
      uint32_t a = addr + (uint32_t)i;
      uint8_t cmd[7] = {'R', (uint8_t)a, (uint8_t)(a >> 8), (uint8_t)(a >> 16), (uint8_t)(a >> 24), (uint8_t)n,
                        (uint8_t)(n >> 8)};
      t_->write(cmd, 7);
      for (size_t done = 0; done < n;) {
        size_t m = n - done < 64 ? n - done : 64;
        if (!t_->read(r, 4 * m, 500)) return false;
        for (size_t k = 0; k < m; k++)
          out[i + done + k] = r[4 * k] | r[4 * k + 1] << 8 | r[4 * k + 2] << 16 | (uint32_t)r[4 * k + 3] << 24;
        done += m;
      }
      i += n;
    }
    return true;
  }

  /// SDRAM read sample point of the gateware ('L': 0-2 whole clocks, +4
  /// half a clock earlier; see gateware/src/sdram_ctrl.v). The bitstream's
  /// default suits the board; tools/tnv.py calibrate finds it otherwise.
  void setReadLatency(uint8_t lat) {
    uint8_t cmd[2] = {'L', lat};
    t_->write(cmd, 2);
  }

  const ImageInfo& imageInfo() const { return info_; }
  float zScale() const { return info_.z_scale; }
  int maxFrames() const { return (int)info_.max_frames; }
  /// The program runs the flow too: sentences are the flow's input z_p.
  bool hasFlow() const { return info_.has_flow; }

 protected:
  VocoderTransport* t_ = nullptr;
  ImageInfo info_;
};

}  // namespace tnv
