#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "Program.h"
#include "TinyTTS/Mat.h"

namespace tnv {

/// Index of the last op that reads each buffer (-1: never read). Executors
/// free a buffer after that op, so a long sentence doesn't keep all ~100
/// intermediate tensors alive. The program output is never freed.
inline std::vector<int> lastUse(const Program& p) {
  std::vector<int> last(p.buffers.size(), -1);
  for (size_t i = 0; i < p.ops.size(); i++) {
    const Op& op = p.ops[i];
    for (int b : {op.in, op.residual, op.in2, op.in3})
      if (b >= 0) last[b] = (int)i;
  }
  last[p.output] = (int)p.ops.size();
  return last;
}

inline int outputFrames(const Op& op, int tin) {
  if (op.kind == OpKind::kConvTranspose) return (tin - 1) * op.stride - 2 * op.padding + op.k;
  return tin;
}

/// The convolution as a gather: every output (t, co) sums its own window, so
/// outputs are independent -- how the hardware computes it too. A transposed
/// convolution's output t reads input ti for every tap kk with
/// ti * stride - padding + kk == t.
template <typename A, typename X, typename W, typename Emit>
void gatherConv(const Op& op, const X* x, int tin, const W* w, int tout, Emit&& emit) {
  const int cin = op.cin, k = op.k;
#pragma omp parallel for schedule(static)
  for (int t = 0; t < tout; t++) {
    for (int co = 0; co < op.cout; co++) {
      A acc = 0;
      const W* wco = w + (size_t)co * k * cin;
      if (op.kind == OpKind::kConvTranspose) {
        int base = t + op.padding;
        for (int kk = base % op.stride; kk < k; kk += op.stride) {
          int ti = (base - kk) / op.stride;
          if (ti < 0 || ti >= tin) continue;
          const X* xr = x + (size_t)ti * cin;
          const W* wr = wco + (size_t)kk * cin;
          for (int ci = 0; ci < cin; ci++) acc += (A)wr[ci] * (A)xr[ci];
        }
      } else {
        for (int kk = 0; kk < k; kk++) {
          int ti = t - op.padding + kk * op.dilation;
          if (ti < 0 || ti >= tin) continue;
          const X* xr = x + (size_t)ti * cin;
          const W* wr = wco + (size_t)kk * cin;
          for (int ci = 0; ci < cin; ci++) acc += (A)wr[ci] * (A)xr[ci];
        }
      }
      emit(t, co, acc);
    }
  }
}

/**
 * @brief Float executor: the reference the integer model is measured
 * against, and the source of calibration statistics. Mirrors
 * tinytts::Vocoder::forward() (checked against it in main.cpp).
 */
class FloatExecutor {
 public:
  explicit FloatExecutor(const Program& p) : p_(p), bufs_(p.buffers.size()), last_(lastUse(p)) {}

  void setInput(const tinytts::Mat& z) {
    frames_ = z.rows();
    bufs_[p_.input].assign(z.data().begin(), z.data().end());
  }

  int frames(int buffer) const { return frames_ * p_.buffers[buffer].rate; }
  const std::vector<float>& buf(int buffer) const { return bufs_[buffer]; }

  void step(size_t i) {
    const Op& op = p_.ops[i];
    int tin = frames(op.in);
    std::vector<float>& y = bufs_[op.out];
    if (op.kind == OpKind::kSum3) {
      const auto &a = bufs_[op.in], &b = bufs_[op.in2], &c = bufs_[op.in3];
      y.resize(a.size());
      for (size_t n = 0; n < a.size(); n++) y[n] = (a[n] + b[n] + c[n]) / 3.0f;
    } else {
      std::vector<float> x = bufs_[op.in];
      if (op.leaky)
        for (float& v : x) v = v < 0 ? v * op.slope : v;
      int tout = outputFrames(op, tin);
      y.assign((size_t)tout * op.cout, 0.0f);
      const float* res = op.residual >= 0 ? bufs_[op.residual].data() : nullptr;
      bool tanh_out = op.kind == OpKind::kOutput;
      gatherConv<float>(op, x.data(), tin, op.w.data(), tout, [&](int t, int co, float acc) {
        float v = acc + op.bias[co];
        if (res) v += res[(size_t)t * op.cout + co];
        y[(size_t)t * op.cout + co] = tanh_out ? std::tanh(v) : v;
      });
    }
    release(i);
  }

  void run() {
    for (size_t i = 0; i < p_.ops.size(); i++) step(i);
  }

 private:
  void release(size_t i) {
    for (size_t b = 0; b < bufs_.size(); b++)
      if (last_[b] == (int)i) std::vector<float>().swap(bufs_[b]);
  }

  const Program& p_;
  std::vector<std::vector<float>> bufs_;
  std::vector<int> last_;
  int frames_ = 0;
};

/// Rounding right shift (round half up), as the hardware does it: add half,
/// arithmetic shift. s >= 1.
inline int64_t roundShift(int64_t v, int s) { return (v + ((int64_t)1 << (s - 1))) >> s; }

inline int32_t clampQ(int64_t v, int32_t qmax, uint64_t& saturated) {
  if (v > qmax) {
    saturated++;
    return qmax;
  }
  if (v < -qmax) {
    saturated++;
    return -qmax;
  }
  return (int32_t)v;
}

/// Leaky ReLU on an integer activation: negative values times a Q15 slope.
inline int32_t leakyInt(int32_t q, int32_t mul) { return q >= 0 ? q : (int32_t)roundShift((int64_t)q * mul, 15); }

/// tanh for kOutput: input Q12 (|x| < 8), 256 segments of 1/32 with linear
/// interpolation, output Q15 (scale 1/32767). 257 table entries -- one
/// block RAM in hardware. Max error about 1 LSB.
inline const std::array<int32_t, 257>& tanhLut() {
  static const std::array<int32_t, 257> lut = [] {
    std::array<int32_t, 257> t{};
    for (int i = 0; i <= 256; i++) t[i] = (int32_t)std::lround(std::tanh(i / 32.0) * 32767.0);
    return t;
  }();
  return lut;
}

inline int32_t tanhQ12ToQ15(int32_t x) {
  const auto& lut = tanhLut();
  int32_t a = x < 0 ? -x : x;  // |x| <= 32767
  int32_t idx = a >> 7, frac = a & 127;
  int32_t y = lut[idx] + (int32_t)roundShift((int64_t)(lut[idx + 1] - lut[idx]) * frac, 7);
  return x < 0 ? -y : y;
}

struct OpStats {
  int64_t max_acc = 0;     ///< largest |accumulator| (before requantize)
  uint64_t saturated = 0;  ///< outputs clamped to the group's range
  uint64_t outputs = 0;
};

/**
 * @brief Integer executor: exactly the arithmetic the gateware does. Needs a
 * quantized program (quantize() in Quantize.h). Every buffer holds integers
 * within its group's bit width; accumulators are tracked as 64 bit here so
 * OpStats can report how many bits they actually need.
 */
class IntExecutor {
 public:
  explicit IntExecutor(const Program& p)
      : p_(p), bufs_(p.buffers.size()), last_(lastUse(p)), stats_(p.ops.size()) {}

  /// Quantizes z to the link format (group "z") -- what the ESP32 sends.
  void setInput(const tinytts::Mat& z) {
    const ScaleGroup& g = p_.groups[p_.buffers[p_.input].group];
    std::vector<int32_t> q(z.data().size());
    uint64_t sat = 0;
    for (size_t n = 0; n < q.size(); n++) q[n] = clampQ(std::lround(z.data()[n] / g.scale), g.qmax(), sat);
    setInputQ(q, z.rows());
  }

  void setInputQ(const std::vector<int32_t>& zq, int frames) {
    frames_ = frames;
    bufs_[p_.input] = zq;
  }

  int frames(int buffer) const { return frames_ * p_.buffers[buffer].rate; }
  const std::vector<int32_t>& buf(int buffer) const { return bufs_[buffer]; }
  const std::vector<OpStats>& stats() const { return stats_; }

  void step(size_t i) {
    const Op& op = p_.ops[i];
    OpStats& st = stats_[i];
    int32_t qmax = p_.groups[p_.buffers[op.out].group].qmax();
    int tin = frames(op.in);
    std::vector<int32_t>& y = bufs_[op.out];

    if (op.kind == OpKind::kSum3) {
      const auto &a = bufs_[op.in], &b = bufs_[op.in2], &c = bufs_[op.in3];
      y.resize(a.size());
      for (size_t n = 0; n < a.size(); n++) {
        int64_t acc = (int64_t)a[n] + b[n] + c[n];
        st.max_acc = std::max<int64_t>(st.max_acc, std::llabs(acc));
        y[n] = clampQ(roundShift(acc * op.mult[0], op.shift[0]), qmax, st.saturated);
      }
      st.outputs += y.size();
      release(i);
      return;
    }

    std::vector<int32_t> x = bufs_[op.in];
    if (op.lr_mul != 32768)
      for (int32_t& v : x) v = leakyInt(v, op.lr_mul);
    int tout = outputFrames(op, tin);
    std::vector<int64_t> acc((size_t)tout * op.cout);
    gatherConv<int64_t>(op, x.data(), tin, op.wq.data(), tout,
                        [&](int t, int co, int64_t a) { acc[(size_t)t * op.cout + co] = a + op.bq[co]; });

    y.resize(acc.size());
    const int32_t* res = op.residual >= 0 ? bufs_[op.residual].data() : nullptr;
    for (size_t n = 0; n < acc.size(); n++) {
      int co = (int)(n % op.cout);
      st.max_acc = std::max<int64_t>(st.max_acc, std::llabs(acc[n]));
      int64_t v = roundShift(acc[n] * op.mult[co], op.shift[co]);
      if (op.kind == OpKind::kOutput) {
        uint64_t sat = 0;  // tanh(+-8) is 1.0 at Q15: clamping the input here is not an error
        y[n] = clampQ(tanhQ12ToQ15(clampQ(v, 32767, sat)), qmax, st.saturated);
        continue;
      }
      if (res) {
        // Requantized to the stream's range first, then added: both steps
        // saturate, as in hardware.
        v = clampQ(v, qmax, st.saturated) + (int64_t)res[n];
      }
      y[n] = clampQ(v, qmax, st.saturated);
    }
    st.outputs += y.size();
    release(i);
  }

  void run() {
    for (size_t i = 0; i < p_.ops.size(); i++) step(i);
  }

 private:
  void release(size_t i) {
    for (size_t b = 0; b < bufs_.size(); b++)
      if (last_[b] == (int)i) std::vector<int32_t>().swap(bufs_[b]);
  }

  const Program& p_;
  std::vector<std::vector<int32_t>> bufs_;
  std::vector<int> last_;
  std::vector<OpStats> stats_;
  int frames_ = 0;
};

}  // namespace tnv
