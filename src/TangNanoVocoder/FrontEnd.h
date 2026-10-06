#pragma once
#include <string>

#include "TangNanoVocoder/Latent.h"
#include "TinyTTS/CmuDict.h"
#include "TinyTTS/DataBuffer.h"
#include "TinyTTS/DictionaryModel.h"
#include "TinyTTS/DurationPredictor.h"
#include "TinyTTS/PhonemeEncoder.h"
#include "TinyTTS/TextG2P.h"
#include "TinyTTS/WeightStore.h"

namespace tnv {

/**
 * @brief The part of TinyTTS the MCU runs when the FPGA runs the flow and
 * the vocoder: G2P, text encoder and duration predictor, text -> z_p. Needs
 * only their weights (front-end weights, 0.66MB:
 * TangNanoVocoder/data/default_frontend_weights.h) instead of all of
 * TinyTTS's (2.07MB), and no memory for the flow's attention.
 *
 *   tnv::FrontEnd fe;
 *   fe.setWeights(default_frontend_weights, default_frontend_weights_len);
 *   fe.setDictionary(default_cmudict_slim, default_cmudict_slim_len);
 *   fe.begin();
 *   tnv::Latent zp = fe.latent("Hello world!");
 */
class FrontEnd {
 public:
  /// Data buffers (flash arrays, or loaded from a File on Arduino); they
  /// must outlive this object.
  void setWeights(const uint8_t* data, size_t len) { weights_buf_.setBorrowed(data, len); }
  void setDictionary(const uint8_t* data, size_t len) { dict_buf_.setBorrowed(data, len); }
  /// Optional: the G2P model for words not in the dictionary.
  void setDictionaryModel(const uint8_t* data, size_t len) { dict_model_buf_.setBorrowed(data, len); }
#ifdef ARDUINO
  bool setWeights(File& file) { return weights_buf_.loadFromFile(file); }
  bool setDictionary(File& file) { return dict_buf_.loadFromFile(file); }
  bool setDictionaryModel(File& file) { return dict_model_buf_.loadFromFile(file); }
#endif

  /// Parses the data; false if weights or dictionary are missing or
  /// malformed, or the weights lack the text encoder or duration predictor.
  bool begin(int n_heads = 2, int window_size = 4) {
    started_ = false;
    if (!weights_.begin(weights_buf_.data(), weights_buf_.size())) return false;
    if (!weights_.has("emb_g.weight") || !weights_.has("dp.proj.weight") ||
        !weights_.has("enc_p.proj.weight"))
      return false;
    if (!dict_.begin(dict_buf_.data(), dict_buf_.size())) return false;
    const tinytts::DictionaryModel* dm = nullptr;
    if (dict_model_buf_.data() != nullptr && dict_model_buf_.size() > 0) {
      if (!dict_model_.begin(dict_model_buf_.data(), dict_model_buf_.size())) return false;
      dm = &dict_model_;
    }
    g2p_.begin(dict_, dm);
    encoder_.begin(weights_, n_heads, window_size);
    dp_.begin(weights_);
    started_ = true;
    return true;
  }

  /// Text -> z_p [frames, 32] and the speaker embedding.
  Latent latent(const std::string& text, int speaker_id = 0, float noise_scale = 0.667f,
                float length_scale = 1.0f, uint32_t rng_seed = 0) const {
    return latentPrior(g2p_, weights_, encoder_, dp_, text, speaker_id, noise_scale, length_scale, rng_seed);
  }

  bool started() const { return started_; }

 protected:
  tinytts::DataBuffer<> weights_buf_, dict_buf_, dict_model_buf_;
  tinytts::WeightStore weights_;
  tinytts::CmuDict dict_;
  tinytts::DictionaryModel dict_model_;
  tinytts::TextG2P g2p_;
  tinytts::PhonemeEncoder encoder_;
  tinytts::DurationPredictor dp_;
  bool started_ = false;
};

}  // namespace tnv
