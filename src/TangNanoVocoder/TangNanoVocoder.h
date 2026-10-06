#pragma once
#include <string>
#include <vector>

#include "TangNanoVocoder/FrontEnd.h"
#include "TangNanoVocoder/Latent.h"
#include "TangNanoVocoder/VocoderClient.h"

namespace tnv {

/**
 * @brief Text to speech with a Tang Nano 20K: this MCU runs TinyTTS's front
 * end (G2P, text encoder, duration predictor: text -> z_p), the FPGA runs
 * the flow and the vocoder and plays the audio over I2S (or its PWM pin).
 * The link (SPI or UART) carries z_p only. speak() returns once the
 * sentence is sent; the FPGA computes and plays it while the next one is
 * prepared.
 *
 *   tnv::SPITransport link(SPI, 10);
 *   tnv::TangNanoVocoder vocoder;
 *   vocoder.setWeights(default_frontend_weights, default_frontend_weights_len);
 *   vocoder.setDictionary(default_cmudict_slim, default_cmudict_slim_len);
 *   vocoder.setImage(default_vocoder_image, default_vocoder_image_len);
 *   vocoder.begin(link);                   // uploads the program image if needed
 *   vocoder.speak("Hello world!");
 *
 * Needs a program image with the flow (the default image). For a
 * vocoder-only image the MCU would have to run the flow itself: use
 * VocoderClient with latentFromText() and all of TinyTTS's weights.
 */
class TangNanoVocoder {
 public:
  /// Front-end data (FrontEnd): TangNanoVocoder/data/default_frontend_weights.h
  /// and TinyTTS's dictionary headers, or Files. Must outlive this object.
  void setWeights(const uint8_t* data, size_t len) { front_.setWeights(data, len); }
  void setDictionary(const uint8_t* data, size_t len) { front_.setDictionary(data, len); }
  void setDictionaryModel(const uint8_t* data, size_t len) { front_.setDictionaryModel(data, len); }
#ifdef ARDUINO
  bool setWeights(File& file) { return front_.setWeights(file); }
  bool setDictionary(File& file) { return front_.setDictionary(file); }
  bool setDictionaryModel(File& file) { return front_.setDictionaryModel(file); }
#endif

  /// The FPGA's program image (TangNanoVocoder/data/default_vocoder_image.h),
  /// uploaded by begin() unless the FPGA already runs it. Optional: without
  /// it, begin() uses the program the FPGA loaded from its own flash
  /// (tools/tnv.py flash-image) - and the sketch is 1.6MB smaller. Must
  /// outlive this object.
  void setImage(const uint8_t* image, size_t len) {
    image_ = image;
    image_len_ = len;
  }

  void setNoiseScale(float s) { noise_scale_ = s; }
  void setLengthScale(float s) { length_scale_ = s; }
  void setSeed(uint32_t seed) { seed_ = seed; }

  /// Why begin() failed, or "" after a successful one.
  const char* error() const { return error_; }

  /// Starts the front end, finds the FPGA, and uploads the program image
  /// unless the FPGA already runs it (or, without an image, uses the
  /// program the FPGA loaded from its flash).
  bool begin(VocoderTransport& link, VocoderClient::ProgressFn progress = nullptr) {
    error_ = "";
    if (!front_.begin()) return fail("front end: weights or dictionary missing or invalid");
    if (image_ != nullptr) {
      ImageInfo info = ImageInfo::parse(image_, image_len_);
      if (!info.valid) return fail("program image invalid");
      if (!info.has_flow) return fail("program image without the flow (see class doc)");
    }
    if (!client_.begin(link)) return fail("the FPGA does not answer");
    if (image_ != nullptr) {
      if (!client_.uploadProgramIfNeeded(image_, image_len_, progress)) return fail("program upload failed");
    } else if (!client_.useLoadedProgram()) {
      return fail("the FPGA has no program (none in its flash) and no image was set");
    }
    if (!client_.hasFlow()) return fail("the FPGA's program has no flow (see class doc)");
    return true;
  }

  /// Text -> z_p -> the FPGA. Returns once the sentence is sent (waiting
  /// while the FPGA's two sentence slots are full). Longer texts should be
  /// split into sentences: one may have at most maxFrames() latent frames
  /// (384, about 4.5s of audio).
  bool speak(const std::string& text) {
    Latent latent = front_.latent(text, 0, noise_scale_, length_scale_, seed_);
    last_frames_ = latent.z.rows();
    if (last_frames_ > maxFrames()) return false;
    std::vector<int16_t> zq = quantizeLatent(latent.z, client_.zScale());
    return client_.sendSentence(zq.data(), last_frames_);
  }

  /// Waits until everything has been computed and played.
  bool waitDone(uint32_t timeout_ms = 120000) { return client_.waitDone(timeout_ms); }

  /// Latent frames of the last sentence (512 audio samples each).
  int lastFrames() const { return last_frames_; }
  int maxFrames() const { return client_.maxFrames(); }
  VocoderClient& client() { return client_; }
  FrontEnd& frontEnd() { return front_; }

 protected:
  FrontEnd front_;
  VocoderClient client_;
  const uint8_t* image_ = nullptr;
  size_t image_len_ = 0;
  float noise_scale_ = 0.667f;
  float length_scale_ = 1.0f;
  uint32_t seed_ = 0;
  int last_frames_ = 0;
  const char* error_ = "";

  bool fail(const char* why) {
    error_ = why;
    return false;
  }
};

}  // namespace tnv
