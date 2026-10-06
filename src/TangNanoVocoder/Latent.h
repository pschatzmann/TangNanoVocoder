#pragma once
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "TinyTTS/Alignment.h"
#include "TinyTTS/TinyTTSCore.h"

namespace tnv {

/// The vocoder's input: the flow's output z [T, 32] plus the speaker embedding.
struct Latent {
  tinytts::Mat z;
  std::vector<float> g;
};

/**
 * @brief Text -> latent: TinyTTSCore::synthesize() up to (not including)
 * the vocoder call - the half of TinyTTS the ESP32 runs in this project.
 * Same steps and the same RNG use, so a given seed gives the same z as the
 * host model (model/main.cpp) and as TinyTTS itself.
 */
inline Latent latentFromText(const tinytts::TinyTTSCore& core, const std::string& text, int speaker_id = 0,
                             float noise_scale = 0.667f, float length_scale = 1.0f, uint32_t rng_seed = 0) {
  using namespace tinytts;
  G2POutput g2p_out = core.g2p().process(text);
  std::vector<int> phone_ids = TextG2P::insertBlanks(g2p_out.phone_ids);
  std::vector<int> tone_ids = TextG2P::insertBlanks(g2p_out.tone_ids);
  std::vector<int> language_ids = TextG2P::insertBlanks(g2p_out.language_ids);

  Mat emb_g = core.weights().embedding("emb_g.weight");
  Mat g(1, emb_g.cols());
  for (int c = 0; c < emb_g.cols(); c++) g.at(0, c) = emb_g.at(speaker_id, c);

  PhonemeEncoderOutput enc_out = core.encoder().forward(phone_ids, tone_ids, language_ids, g);
  std::vector<float> logw = core.durationPredictor().forward(enc_out.x, g);
  std::vector<int> durations = alignment::durationsFromLogw(logw, length_scale);
  int t_y = alignment::totalDuration(durations);
  Mat m_p_exp = alignment::expandByDuration(enc_out.m_p, durations, t_y);
  Mat logs_p_exp = alignment::expandByDuration(enc_out.logs_p, durations, t_y);

  std::mt19937 rng(rng_seed);
  std::normal_distribution<float> normal(0.0f, 1.0f);
  Mat z_p(t_y, m_p_exp.cols());
  for (int t = 0; t < t_y; t++)
    for (int c = 0; c < m_p_exp.cols(); c++)
      z_p.at(t, c) = m_p_exp.at(t, c) + normal(rng) * std::exp(logs_p_exp.at(t, c)) * noise_scale;

  Latent out;
  out.z = core.flow().reverse(z_p, g);
  out.g.assign(g.row(0), g.row(0) + g.cols());
  return out;
}

/**
 * @brief The latent as the vocoder's link carries it: int16, frame by
 * frame (32 channels), value = round(z / z_scale) clamped to +-32767 -
 * exactly what the fixed-point model does (Executors.h, IntExecutor). The
 * scale is in the hardware image's header (ImageInfo::z_scale).
 */
inline std::vector<int16_t> quantizeLatent(const tinytts::Mat& z, float z_scale) {
  std::vector<int16_t> q((size_t)z.rows() * z.cols());
  for (size_t n = 0; n < q.size(); n++) {
    long v = std::lround(z.data()[n] / z_scale);
    q[n] = (int16_t)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v));
  }
  return q;
}

}  // namespace tnv
