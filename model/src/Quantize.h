#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "Executors.h"
#include "Program.h"

namespace tnv {

/**
 * @brief Per scale group statistics of the float activations, collected by
 * running FloatExecutor over calibration sentences. Keeps the max and a
 * log2 histogram of |x|, so a percentile range can be chosen afterwards
 * without storing the activations.
 */
class Calibrator {
 public:
  static constexpr int kBins = 4096;
  static constexpr float kLog2Min = -24.0f, kLog2Max = 16.0f;

  explicit Calibrator(const Program& p) : p_(p), max_(p.groups.size(), 0.0f), hist_(p.groups.size()) {
    for (auto& h : hist_) h.assign(kBins, 0);
  }

  void add(int group, const std::vector<float>& v) {
    float& m = max_[group];
    auto& h = hist_[group];
    for (float x : v) {
      float a = std::fabs(x);
      m = std::max(m, a);
      if (a <= 0.0f) continue;
      int bin = (int)((std::log2(a) - kLog2Min) / (kLog2Max - kLog2Min) * kBins);
      h[std::clamp(bin, 0, kBins - 1)]++;
    }
  }

  /// Runs the float program on one latent and records every buffer.
  void addSentence(const tinytts::Mat& z) {
    FloatExecutor fx(p_);
    fx.setInput(z);
    add(p_.buffers[p_.input].group, fx.buf(p_.input));
    for (size_t i = 0; i < p_.ops.size(); i++) {
      fx.step(i);
      int out = p_.ops[i].out;
      add(p_.buffers[out].group, fx.buf(out));
    }
  }

  float max(int group) const { return max_[group]; }

  /// Smallest range covering `pct` percent of the group's values.
  float percentile(int group, double pct) const {
    const auto& h = hist_[group];
    uint64_t total = 0;
    for (uint64_t c : h) total += c;
    if (total == 0) return 0.0f;
    uint64_t target = (uint64_t)std::ceil(total * pct / 100.0), seen = 0;
    for (int b = 0; b < kBins; b++) {
      seen += h[b];
      if (seen >= target)
        return std::min(max_[group], std::exp2(kLog2Min + (b + 1) * (kLog2Max - kLog2Min) / kBins));
    }
    return max_[group];
  }

 private:
  const Program& p_;
  std::vector<float> max_;
  std::vector<std::vector<uint64_t>> hist_;
};

/// Sets every non-fixed group's scale from calibration: range / qmax, where
/// range is the max (pct >= 100) or the given percentile of |x|, times
/// `headroom` (calibration sentences don't contain every peak).
inline void applyCalibration(Program& p, const Calibrator& cal, double pct, double headroom = 1.0) {
  for (size_t g = 0; g < p.groups.size(); g++) {
    ScaleGroup& sg = p.groups[g];
    if (sg.fixed) continue;
    float range = pct >= 100.0 ? cal.max((int)g) : cal.percentile((int)g, pct);
    if (range <= 0.0f) range = 1.0f;
    range *= (float)headroom;
    sg.scale = range / (float)sg.qmax();
  }
}

/// real multiplier m > 0 -> (mult, shift) with mult in [2^14, 2^15) and
/// m ~= mult / 2^shift: 15 significant bits for every channel.
inline void toMultShift(double m, int32_t& mult, int32_t& shift) {
  if (!(m > 0.0)) throw std::runtime_error("requantize multiplier must be positive");
  int e;
  std::frexp(m, &e);  // m = f * 2^e, f in [0.5, 1)
  shift = 15 - e;
  mult = (int32_t)std::llround(std::ldexp(m, shift));
  if (mult == 32768) {
    mult = 16384;
    shift--;
  }
  if (shift < 1 || shift > 62) throw std::runtime_error("requantize shift out of range");
}

/**
 * @brief Quantizes weights, biases and requantize multipliers for the
 * current group scales. Weights: symmetric, one scale per output channel.
 * For Conv1d, 8 bits reproduce TinyTTS's stored INT8 weights exactly (its
 * per-row scale is per output channel). ConvTranspose1d is stored per
 * *input* channel in TinyTTS, but the scale has to fold into the output's
 * requantize step, so it is requantized per output channel: at 8 bits that
 * double quantization costs about 45dB at the first upsampling layer, so
 * those layers get `convt_wbits` (they are under 5% of the MACs).
 */
inline void quantize(Program& p, int convt_wbits = 12) {
  for (Op& op : p.ops) {
    op.wbits = op.kind == OpKind::kConvTranspose ? convt_wbits : 8;
    float sx = p.groups[p.buffers[op.in].group].scale;
    float sy = op.kind == OpKind::kOutput ? std::ldexp(1.0f, -kTanhInFracBits) : p.groups[p.buffers[op.out].group].scale;
    op.lr_mul = op.leaky ? (int32_t)std::lround(op.slope * 32768.0f) : 32768;

    if (op.kind == OpKind::kSum3) {
      for (int b : {op.in2, op.in3})
        if (p.buffers[b].group != p.buffers[op.in].group) throw std::runtime_error(op.name + ": inputs differ in scale");
      int32_t m, s;
      toMultShift((double)sx / (3.0 * sy), m, s);
      op.mult.assign(op.cout, m);
      op.shift.assign(op.cout, s);
      continue;
    }
    if (op.residual >= 0 && p.buffers[op.residual].group != p.buffers[op.out].group)
      throw std::runtime_error(op.name + ": residual and output differ in scale");

    size_t row = (size_t)op.k * op.cin;
    op.wq.resize(op.w.size());
    op.wscale.resize(op.cout);
    op.bq.resize(op.cout);
    op.mult.resize(op.cout);
    op.shift.resize(op.cout);
    for (int co = 0; co < op.cout; co++) {
      const float* w = op.w.data() + co * row;
      float m = 0.0f;
      for (size_t n = 0; n < row; n++) m = std::max(m, std::fabs(w[n]));
      long wmax = (1L << (op.wbits - 1)) - 1;
      float ws = m > 0.0f ? m / (float)wmax : 1.0f;
      for (size_t n = 0; n < row; n++) op.wq[co * row + n] = (int16_t)std::clamp<long>(std::lround(w[n] / ws), -wmax, wmax);
      op.wscale[co] = ws;
      op.bq[co] = (int32_t)std::llround((double)op.bias[co] / ((double)ws * sx));
      toMultShift((double)ws * sx / sy, op.mult[co], op.shift[co]);
    }
  }
}

}  // namespace tnv
