#pragma once
#include <string>
#include <vector>

#include "TangNanoVocoder/Latent.h"
#include "TangNanoVocoder/VocoderClient.h"
#include "TinyTTS.h"

namespace tnv {

/**
 * @brief Text to speech with the vocoder on a Tang Nano 20K: TinyTTS runs
 * up to the flow on this MCU (ESP32-S3 with PSRAM), the latent goes over
 * SPI or UART, and the FPGA runs the vocoder and plays the audio over I2S
 * (or its PWM pin). speak() returns once the sentence is sent; the FPGA
 * computes and plays it while the next one is prepared.
 *
 *   tinytts::TinyTTS tts;                  // weights and dictionary as usual
 *   tnv::SPITransport link(SPI, 10);
 *   tnv::TangNanoVocoder vocoder(tts);
 *   vocoder.begin(link);                   // uploads the program image if needed
 *   vocoder.speak("Hello world!");
 */
class TangNanoVocoder {
 public:
  explicit TangNanoVocoder(tinytts::TinyTTS& tts) : tts_(tts) {}

  /// The hardware image to upload (default: default_vocoder_image, if
  /// TangNanoVocoder/data/default_vocoder_image.h is included). Must
  /// outlive this object.
  void setImage(const uint8_t* image, size_t len) {
    image_ = image;
    image_len_ = len;
  }

  void setSpeakerId(int id) { speaker_id_ = id; }
  void setNoiseScale(float s) { noise_scale_ = s; }
  void setLengthScale(float s) { length_scale_ = s; }
  void setSeed(uint32_t seed) { seed_ = seed; }

  /// Starts TinyTTS (its weights and dictionary must be set), finds the
  /// vocoder and uploads the program image unless it has one.
  bool begin(VocoderTransport& link, VocoderClient::ProgressFn progress = nullptr) {
    if (!tts_.begin([](const float*, size_t) {})) return false;  // only its core is used
    if (!client_.begin(link)) return false;
    if (image_ == nullptr) return false;
    return client_.uploadProgramIfNeeded(image_, image_len_, progress);
  }

  /// Text -> latent (TinyTTS) -> vocoder. Returns once the sentence is sent
  /// (waiting while the vocoder's two latent slots are full). Longer texts
  /// should be split into sentences: one may have at most maxFrames()
  /// latent frames (about 5s of audio).
  bool speak(const std::string& text) {
    Latent latent = latentFromText(tts_.core(), text, speaker_id_, noise_scale_, length_scale_, seed_);
    last_frames_ = latent.z.rows();
    std::vector<int16_t> zq = quantizeLatent(latent.z, client_.zScale());
    return client_.sendSentence(zq.data(), last_frames_);
  }

  /// Waits until everything has been computed and played.
  bool waitDone(uint32_t timeout_ms = 120000) { return client_.waitDone(timeout_ms); }

  /// Latent frames of the last sentence (512 audio samples each).
  int lastFrames() const { return last_frames_; }
  int maxFrames() const { return client_.maxFrames(); }
  VocoderClient& client() { return client_; }

 protected:
  tinytts::TinyTTS& tts_;
  VocoderClient client_;
  const uint8_t* image_ = nullptr;
  size_t image_len_ = 0;
  int speaker_id_ = 0;
  float noise_scale_ = 0.667f;
  float length_scale_ = 1.0f;
  uint32_t seed_ = 0;
  int last_frames_ = 0;
};

}  // namespace tnv
