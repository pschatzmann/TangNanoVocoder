#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Executors.h"  // roundShift

namespace tnv {

/**
 * @brief The integer definitions of the flow's LayerNorm and attention, as
 * the FPGA computes them (docs/studies.md, "The flow in fixed point"). Shared by the model
 * (FlowFixed.h), the hardware image interpreter (HwSim.h) and, through
 * them, the gateware tests.
 */
struct FlowTables {
  int act_bits = 16;
  int exp_bits = 8;     ///< exp2 table: 2^exp_bits segments over [0, 1)
  int rsqrt_bits = 8;   ///< 1/sqrt table: 2^rsqrt_bits segments over [1, 2)
  int score_frac = 11;  ///< fraction bits of the log2-domain softmax input
  std::vector<int64_t> exp_lut, rsqrt_lut;  ///< Q16, 2^bits + 1 entries each

  void init() {
    int n = 1 << exp_bits;
    exp_lut.resize(n + 1);
    for (int i = 0; i <= n; i++) exp_lut[i] = std::lround(std::exp2(-(double)i / n) * 65536.0);
    n = 1 << rsqrt_bits;
    rsqrt_lut.resize(n + 1);
    for (int i = 0; i <= n; i++) rsqrt_lut[i] = std::lround(65536.0 / std::sqrt(1.0 + (double)i / n));
  }
  int32_t qmax() const { return (int32_t)((1u << (act_bits - 1)) - 1); }
};

/// LayerNorm parameters: y = u * rsqrt(W) * g / 2^gs + b (see layerNormFrame)
struct NormParams {
  int64_t eps = 0;         ///< added to W = sum(u^2) / 32
  std::vector<int64_t> g;  ///< gamma / s_out * 2^gs, 16-bit signed
  std::vector<int64_t> b;  ///< beta / s_out
  int gs = 0;
};

/// Attention parameters of one layer (all heads)
struct AttnParams {
  int heads = 2, head_dim = 16, window = 4;
  int32_t score_mult = 0, score_shift = 0;  ///< q.k accumulator -> log2 units, score_frac fraction bits
  int32_t merge_mult = 0, merge_shift = 0;  ///< sum p (Q15) x v -> output group
  std::vector<int32_t> rel_k, rel_v;        ///< [2 window + 1][head_dim], in k's / v's units
};

namespace flowops {

inline int64_t shiftRound(int64_t v, int s) { return s > 0 ? roundShift(v, s) : v << (-s); }

inline int32_t clampTo(int64_t v, int32_t qmax, uint64_t* sat) {
  if (v > qmax || v < -qmax) {
    if (sat) (*sat)++;
    return v > qmax ? qmax : -qmax;
  }
  return (int32_t)v;
}

/// 2^(x / 2^score_frac) for x <= 0, Q16: table for the fraction, shift for the integer part
inline int64_t exp2Q(int64_t x, const FlowTables& t) {
  int64_t nx = -x;
  int fb = t.score_frac;
  int64_t ip = nx >> fb, fp = nx & ((1 << fb) - 1);
  if (ip >= 31) return 0;
  int rb = fb - t.exp_bits;  // interpolation bits
  int64_t idx = rb >= 0 ? fp >> rb : fp << (-rb);
  int64_t rem = rb > 0 ? fp & ((1 << rb) - 1) : 0;
  int64_t a = t.exp_lut[idx], b = t.exp_lut[idx + 1];
  int64_t v = a + (rb > 0 ? roundShift((b - a) * rem, rb) : 0);
  return v >> ip;
}

/**
 * LayerNorm of one frame of C = 32 values, in place: u = 32 x - sum(x)
 * (exact), W = sum(u^2) / 32 + eps, R = 1/sqrt(m) from the table for the
 * mantissa m of W in [1, 2) (interpolated with a 16-bit fraction; times
 * 1/sqrt(2) for an odd exponent), then two
 * multiplies that fit DSP blocks: nq = u R (Q14 normalized value, |nq| <=
 * sqrt(32) 2^14), y = nq g / 2^(14 + gs) + b.
 */
inline void layerNormFrame(int32_t* r, const NormParams& n, const FlowTables& t, uint64_t* sat) {
  const int C = 32;
  int64_t sum = 0;
  for (int c = 0; c < C; c++) sum += r[c];
  int64_t u[C], sq = 0;
  for (int c = 0; c < C; c++) {
    u[c] = 32 * (int64_t)r[c] - sum;
    sq += u[c] * u[c];
  }
  int64_t W = (sq >> 5) + n.eps;
  if (W < 1) W = 1;
  int E = 63 - __builtin_clzll((uint64_t)W);
  int lb = t.rsqrt_bits, rb = E - lb;
  int64_t frac = W - ((int64_t)1 << E);
  int64_t idx = rb >= 0 ? frac >> rb : frac << (-rb);
  // interpolation with the remainder as a 16-bit fraction (truncated when
  // longer): (b - a) x rem16 fits a DSP block
  int64_t rem = rb > 0 ? frac & (((int64_t)1 << rb) - 1) : 0;
  int64_t rem16 = rb > 16 ? rem >> (rb - 16) : rem << (16 - (rb > 0 ? rb : 16));
  int64_t a = t.rsqrt_lut[idx], bb = t.rsqrt_lut[idx + 1];
  int64_t R = a + shiftRound((bb - a) * rem16, 16);  // 1/sqrt(m), Q16
  if (E & 1) R = (R * 46341 + (1 << 15)) >> 16;                    // * 1/sqrt(2)
  int half_e = E >> 1;
  for (int c = 0; c < C; c++) {
    int64_t nq = shiftRound(u[c] * R, 16 + half_e - 14);
    int64_t v = shiftRound(nq * n.g[c], 14 + n.gs);
    r[c] = clampTo(v + n.b[c], t.qmax(), sat);
  }
}

/**
 * Attention of one layer over T frames: q, k, v and merged are [T][stride]
 * (stride 32 = heads x head_dim). Per head and query row i: s_j = q_i .
 * (k_j + rel_k[j - i]) (relative terms within +-window), x_j = (s_j - max)
 * score_mult >> score_shift (log2 units, at least -16), e_j = exp2Q(x_j),
 * r = 2^47 / sum(e) (one division per row), p_j = e_j r >> 32 (Q15),
 * merged_i = sum p_j (v_j + rel_v[j - i]) requantized.
 */
inline void attention(const int32_t* q, const int32_t* k, const int32_t* v, int T, int stride, const AttnParams& a,
                      const FlowTables& t, int32_t* merged, uint64_t* sat) {
  int hd = a.head_dim, w = a.window;
  std::vector<int64_t> s(T), e(T);
  int64_t floor_x = -((int64_t)16 << t.score_frac);
  for (int h = 0; h < a.heads; h++) {
    int off = h * hd;
    for (int i = 0; i < T; i++) {
      for (int j = 0; j < T; j++) {
        int64_t acc = 0;
        int d = j - i;
        for (int ch = 0; ch < hd; ch++) {
          int64_t kv = k[(size_t)j * stride + off + ch];
          if (d >= -w && d <= w) kv += a.rel_k[(size_t)(d + w) * hd + ch];
          acc += (int64_t)q[(size_t)i * stride + off + ch] * kv;
        }
        s[j] = acc;
      }
      int64_t mx = *std::max_element(s.begin(), s.end());
      int64_t sum = 0;
      for (int j = 0; j < T; j++) {
        int64_t xs = shiftRound((s[j] - mx) * a.score_mult, a.score_shift);
        e[j] = exp2Q(std::max(xs, floor_x), t);
        sum += e[j];
      }
      int64_t r = ((int64_t)1 << 47) / sum;
      for (int ch = 0; ch < hd; ch++) {
        int64_t acc = 0;
        for (int j = 0; j < T; j++) {
          int64_t p = (e[j] * r + ((int64_t)1 << 31)) >> 32;  // Q15
          int64_t vv = v[(size_t)j * stride + off + ch];
          int d = j - i;
          if (d >= -w && d <= w) vv += a.rel_v[(size_t)(d + w) * hd + ch];
          acc += p * vv;
        }
        merged[(size_t)i * stride + off + ch] = clampTo(shiftRound(acc * a.merge_mult, a.merge_shift), t.qmax(), sat);
      }
    }
  }
}

}  // namespace flowops
}  // namespace tnv
