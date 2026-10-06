#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "Executors.h"
#include "FlowOps.h"
#include "Quantize.h"
#include "TinyTTS/Mat.h"
#include "TinyTTS/WeightStore.h"

namespace tnv {

/// Choices of the fixed-point flow (docs/studies.md, "The flow in fixed point").
struct FlowConfig {
  int act_bits = 16;       ///< activations
  int weight_bits = 8;     ///< weights, symmetric per output channel
  int exp_lut_bits = 8;    ///< softmax: 2^-f table over f in [0, 1), 2^bits segments
  int rsqrt_lut_bits = 8;  ///< LayerNorm: 1/sqrt(m) tables over m in [1, 2), 2^bits segments
  int score_frac = 11;     ///< softmax input: fraction bits of the log2-domain score
  float headroom = 1.0f;   ///< calibrated ranges are multiplied by this
};

/**
 * @brief TinyTTS's flow (tinytts::Flow::reverse(): 4 coupling layers, each
 * a 3-layer transformer with windowed relative-position attention) in fixed
 * point, the way the FPGA would compute it, next to the same graph in float
 * (for calibration, and checked against TinyTTS itself).
 *
 * Integer arithmetic (runFixed()):
 *  - activations: signed act_bits integers with one static, calibrated
 *    scale per tensor group; weights per output channel; accumulators exact
 *    (64 bit here), requantized as in the vocoder (Quantize.h, toMultShift);
 *    residual adds after requantizing into the residual's group.
 *  - softmax: the score row minus its maximum, scaled into log2 units with
 *    `score_frac` fraction bits, exp2 of the fraction from a table with
 *    linear interpolation (Q16) shifted by the integer part, one division
 *    per row (r = 2^47 / sum), weights p = e * r >> 32 in Q15.
 *  - LayerNorm over the 32 channels: u = 32 x - sum(x) (exact), W = sum(u^2)
 *    / 32 + E (E holds eps), 1/sqrt(W) as a table value for the mantissa in
 *    [1, 2) and a power of two, y = u * rsqrt * gamma + beta.
 *  - the speaker conditioning is folded into a LayerNorm's beta, the
 *    channel flip into the weights that read the halves, `x1 - m` into the
 *    post projection (negated) plus a residual add.
 */
class FlowFixed {
 public:
  explicit FlowFixed(FlowConfig cfg = FlowConfig()) : cfg_(cfg) {}

  void build(const tinytts::WeightStore& ws, const std::vector<float>& g, int n_flows = 4, int n_layers = 3,
             int n_heads = 2, int window = 4) {
    n_heads_ = n_heads;
    window_ = window;
    groups_.clear();
    z_group_ = addGroup("z");
    couplings_.resize(n_flows);
    for (int f = 0; f < n_flows; f++) {
      Coupling& cp = couplings_[f];
      std::string p = "flow.flows." + std::to_string(f * 2);
      cp.pre = linear(ws, p + ".pre", 1);
      cp.post = linear(ws, p + ".post", 1);
      for (float& v : cp.post.w) v = -v;  // x1 - m as x1 + (-m)
      for (float& v : cp.post.b) v = -v;
      cp.layers.resize(n_layers);
      for (int l = 0; l < n_layers; l++) {
        Layer& L = cp.layers[l];
        std::string lp = p + ".enc.attn_layers." + std::to_string(l);
        L.q = linear(ws, lp + ".conv_q", 1);
        L.k = linear(ws, lp + ".conv_k", 1);
        L.v = linear(ws, lp + ".conv_v", 1);
        L.o = linear(ws, lp + ".conv_o", 1);
        L.rel_k = table(ws, lp + ".emb_rel_k");
        L.rel_v = table(ws, lp + ".emb_rel_v");
        std::string fp = p + ".enc.ffn_layers." + std::to_string(l);
        L.ff1 = linear(ws, fp + ".conv_1", 5);
        L.ff2 = linear(ws, fp + ".conv_2", 5);
        L.ln1_g = vec(ws, p + ".enc.norm_layers_1." + std::to_string(l) + ".gamma");
        L.ln1_b = vec(ws, p + ".enc.norm_layers_1." + std::to_string(l) + ".beta");
        L.ln2_g = vec(ws, p + ".enc.norm_layers_2." + std::to_string(l) + ".gamma");
        L.ln2_b = vec(ws, p + ".enc.norm_layers_2." + std::to_string(l) + ".beta");
        std::string gp = "c" + std::to_string(f) + ".l" + std::to_string(l);
        L.g_x = addGroup(gp + ".x");    // layer input and attention + residual
        L.g_q = addGroup(gp + ".q");
        L.g_k = addGroup(gp + ".k");
        L.g_v = addGroup(gp + ".v");
        L.g_m = addGroup(gp + ".attn");  // merged heads
        L.g_y = addGroup(gp + ".ln1");   // LayerNorm 1 output and FFN + residual
        L.g_f = addGroup(gp + ".ffn");   // ReLU(conv_1)
      }
      cp.g_out = addGroup("c" + std::to_string(f) + ".out");  // last LayerNorm output, post's input
      // speaker conditioning before layer 2: a constant, folded into layer 1's LayerNorm 2 beta
      std::vector<float> spk_w = vec(ws, p + ".enc.spk_emb_linear.weight");  // [32 x 128]
      std::vector<float> spk_b = vec(ws, p + ".enc.spk_emb_linear.bias");
      int hidden = (int)spk_b.size();
      if ((int)spk_w.size() != hidden * (int)g.size()) throw std::runtime_error("speaker embedding size");
      if (n_layers > 2)
        for (int c = 0; c < hidden; c++) {
          float s = spk_b[c];
          for (size_t j = 0; j < g.size(); j++) s += spk_w[(size_t)c * g.size() + j] * g[j];
          cp.layers[1].ln2_b[c] += s;
        }
    }
    initTables();
  }

  /// Float graph: TinyTTS's flow; records group ranges when `calibrating`.
  tinytts::Mat runFloat(const tinytts::Mat& z_p, bool calibrating = false) {
    int T = z_p.rows();
    std::vector<float> x(z_p.data().begin(), z_p.data().end());  // [T][32]
    record(z_group_, x, calibrating);
    for (int f = (int)couplings_.size() - 1; f >= 0; f--) {
      flip(x, T, 32);
      Coupling& cp = couplings_[f];
      std::vector<float> x0 = half(x, T, 0), x1 = half(x, T, 16);
      std::vector<float> h = convF(cp.pre, x0, T);
      for (size_t l = 0; l < cp.layers.size(); l++) {
        Layer& L = cp.layers[l];
        record(L.g_x, h, calibrating);
        std::vector<float> q = convF(L.q, h, T), k = convF(L.k, h, T), v = convF(L.v, h, T);
        record(L.g_q, q, calibrating);
        record(L.g_k, k, calibrating);
        record(L.g_v, v, calibrating);
        std::vector<float> m = attentionF(L, q, k, v, T);
        record(L.g_m, m, calibrating);
        std::vector<float> y = convF(L.o, m, T);
        for (size_t n = 0; n < y.size(); n++) y[n] += h[n];
        record(L.g_x, y, calibrating);
        layerNormF(y, T, L.ln1_g, L.ln1_b);
        record(L.g_y, y, calibrating);
        std::vector<float> ff = convF(L.ff1, y, T);
        for (float& v2 : ff) v2 = std::max(0.0f, v2);
        record(L.g_f, ff, calibrating);
        std::vector<float> ff2 = convF(L.ff2, ff, T);
        for (size_t n = 0; n < ff2.size(); n++) ff2[n] += y[n];
        record(L.g_y, ff2, calibrating);
        layerNormF(ff2, T, L.ln2_g, L.ln2_b);
        h = std::move(ff2);
      }
      record(cp.g_out, h, calibrating);
      std::vector<float> m = convF(cp.post, h, T);  // -m
      for (size_t n = 0; n < m.size(); n++) x1[n] += m[n];
      join(x, x0, x1, T);
      record(z_group_, x, calibrating);
    }
    tinytts::Mat out(T, 32);
    std::copy(x.begin(), x.end(), out.data().begin());
    return out;
  }

  /// Scales from the ranges recorded by runFloat(..., true), then weights,
  /// biases and multipliers.
  /// Range of the latent stream (calibrated, with headroom), to share the
  /// scale with the vocoder's input.
  float zRange() const { return (groups_[z_group_].max > 0 ? groups_[z_group_].max : 1.0f) * cfg_.headroom; }

  /// z_range > 0: the latent stream's range (shared with the vocoder's z).
  void quantize(float z_range = 0) {
    for (Group& g : groups_) {
      float range = (g.max > 0 ? g.max : 1.0f) * cfg_.headroom;
      g.scale = range / (float)qmax();
    }
    if (z_range > 0) groups_[z_group_].scale = z_range / (float)qmax();
    for (Coupling& cp : couplings_) {
      quantizeConv(cp.pre, z_group_, cp.layers[0].g_x);
      for (size_t l = 0; l < cp.layers.size(); l++) {
        Layer& L = cp.layers[l];
        quantizeConv(L.q, L.g_x, L.g_q);
        quantizeConv(L.k, L.g_x, L.g_k);
        quantizeConv(L.v, L.g_x, L.g_v);
        quantizeConv(L.o, L.g_m, L.g_x);
        quantizeConv(L.ff1, L.g_y, L.g_f);
        quantizeConv(L.ff2, L.g_f, L.g_y);
        // relative embeddings in the units of k and v, so they add to them directly
        float sk = groups_[L.g_k].scale, sv = groups_[L.g_v].scale;
        L.attn.heads = n_heads_;
        L.attn.head_dim = headDim();
        L.attn.window = window_;
        L.attn.rel_k.resize(L.rel_k.size());
        L.attn.rel_v.resize(L.rel_v.size());
        for (size_t n = 0; n < L.rel_k.size(); n++) L.attn.rel_k[n] = clampQ(std::lround(L.rel_k[n] / sk));
        for (size_t n = 0; n < L.rel_v.size(); n++) L.attn.rel_v[n] = clampQ(std::lround(L.rel_v[n] / sv));
        // softmax input: score accumulator (q x k units) -> log2 domain with score_frac bits
        double sq = groups_[L.g_q].scale;
        double score_real = sq * sk / std::sqrt((double)headDim());
        toMultShiftAny(score_real / std::log(2.0) * (double)(1 << cfg_.score_frac), L.attn.score_mult,
                       L.attn.score_shift);
        // merged = sum p (Q15) x v -> attention group
        toMultShiftAny(sv / 32768.0 / groups_[L.g_m].scale, L.attn.merge_mult, L.attn.merge_shift);
        int out_ln1 = L.g_y;
        int out_ln2 = l + 1 < cp.layers.size() ? cp.layers[l + 1].g_x : cp.g_out;
        quantizeNorm(L.ln1_g, L.ln1_b, L.g_x, out_ln1, L.n1);
        quantizeNorm(L.ln2_g, L.ln2_b, L.g_y, out_ln2, L.n2);
      }
      quantizeConv(cp.post, cp.g_out, z_group_);
    }
  }

  /// Integer graph; returns z dequantized, and the integers in `zq` (z group).
  tinytts::Mat runFixed(const tinytts::Mat& z_p, std::vector<int32_t>* zq = nullptr) {
    int T = z_p.rows();
    std::vector<int32_t> x(z_p.data().size());
    float zs = groups_[z_group_].scale;
    for (size_t n = 0; n < x.size(); n++) x[n] = clampQ(std::lround(z_p.data()[n] / zs));
    for (int f = (int)couplings_.size() - 1; f >= 0; f--) {
      flip(x, T, 32);
      Coupling& cp = couplings_[f];
      std::vector<int32_t> x0 = half(x, T, 0), x1 = half(x, T, 16);
      std::vector<int32_t> h = convI(cp.pre, x0, T, nullptr, false);
      for (size_t l = 0; l < cp.layers.size(); l++) {
        Layer& L = cp.layers[l];
        std::vector<int32_t> q = convI(L.q, h, T, nullptr, false), k = convI(L.k, h, T, nullptr, false),
                             v = convI(L.v, h, T, nullptr, false);
        std::vector<int32_t> m = attentionI(L, q, k, v, T);
        std::vector<int32_t> y = convI(L.o, m, T, &h, false);
        layerNormI(y, T, L.n1);
        std::vector<int32_t> ff = convI(L.ff1, y, T, nullptr, true);
        std::vector<int32_t> ff2 = convI(L.ff2, ff, T, &y, false);
        layerNormI(ff2, T, L.n2);
        h = std::move(ff2);
      }
      std::vector<int32_t> x1n = convI(cp.post, h, T, &x1, false);  // x1 + (-m)
      join(x, x0, x1n, T);
    }
    if (zq) *zq = x;
    tinytts::Mat out(T, 32);
    for (size_t n = 0; n < x.size(); n++) out.data()[n] = x[n] * zs;
    return out;
  }

  /// First LayerNorm and first attention of the last runFixed(): inputs,
  /// outputs, parameters - golden vectors for the gateware's flow unit.
  struct Capture {
    bool ln_done = false, att_done = false;
    std::vector<int32_t> ln_in, ln_out, q, k, v, merged;
    NormParams norm;
    AttnParams attn;
  };
  Capture capture;

  float zScale() const { return groups_[z_group_].scale; }
  int nHeads() const { return n_heads_; }
  int window() const { return window_; }
  const FlowConfig& config() const { return cfg_; }
  /// range of requantize shifts of the convolutions (the vocoder's post unit takes 15..32)
  std::pair<int, int> convShiftRange() const {
    int lo = 99, hi = -99;
    auto add = [&](const Conv& c) {
      for (int32_t sh : c.shift) {
        lo = std::min(lo, (int)sh);
        hi = std::max(hi, (int)sh);
      }
    };
    for (const Coupling& cp : couplings_) {
      add(cp.pre);
      add(cp.post);
      for (const Layer& L : cp.layers) {
        add(L.q); add(L.k); add(L.v); add(L.o); add(L.ff1); add(L.ff2);
      }
    }
    return {lo, hi};
  }
  /// groups with saturated values, most first
  std::string saturationReport(int top = 8) const {
    std::vector<const Group*> g;
    for (const Group& x : groups_)
      if (x.saturated) g.push_back(&x);
    std::sort(g.begin(), g.end(), [](const Group* a, const Group* b) { return a->saturated > b->saturated; });
    std::string out;
    for (int i = 0; i < (int)g.size() && i < top; i++)
      out += "    " + g[i]->name + ": " + std::to_string(g[i]->saturated) + " (range " + std::to_string(g[i]->max) + ")\n";
    return out;
  }
  uint64_t saturated() const { return saturated_; }
  /// bits (with sign) of the largest convolution accumulator seen in runFixed()
  int accBits() const {
    int b = 1;
    for (int64_t m = max_acc_; m > 0; m >>= 1) b++;
    return b;
  }
  int paramCount() const {
    int n = 0;
    for (const Coupling& cp : couplings_) {
      n += (int)(cp.pre.w.size() + cp.post.w.size());
      for (const Layer& L : cp.layers)
        n += (int)(L.q.w.size() + L.k.w.size() + L.v.w.size() + L.o.w.size() + L.ff1.w.size() + L.ff2.w.size());
    }
    return n;
  }

 public:  // the quantized graph, for the hardware image (HwImage.h)
  struct Conv {
    int cin = 0, cout = 0, k = 1;
    std::vector<float> w;  // [co][kk][ci]
    std::vector<float> b;
    std::vector<int32_t> wq, mult, shift;
    std::vector<int64_t> bq;  // bias in accumulator units (can exceed 32 bits with wide weights)
    int in_group = 0, out_group = 0;
  };
  struct Norm : NormParams {  // integer LayerNorm parameters (FlowOps.h)
    int out_group = 0;
  };
  struct Layer {
    Conv q, k, v, o, ff1, ff2;
    std::vector<float> rel_k, rel_v;  // [2w+1][head dim], shared by the heads
    AttnParams attn;  // integer attention parameters (FlowOps.h), incl. rel_k/rel_v quantized
    std::vector<float> ln1_g, ln1_b, ln2_g, ln2_b;
    Norm n1, n2;
    int g_x = 0, g_q = 0, g_k = 0, g_v = 0, g_m = 0, g_y = 0, g_f = 0;
  };
  struct Coupling {
    Conv pre, post;
    std::vector<Layer> layers;
    int g_out = 0;
  };
  const std::vector<struct Coupling>& couplings() const { return couplings_; }
  const FlowTables& tables() const { return tables_; }

 private:
  struct Group {
    std::string name;
    float max = 0, scale = 1;
    uint64_t saturated = 0;
  };

  int addGroup(const std::string& name) {
    groups_.push_back({name, 0, 1, 0});
    return (int)groups_.size() - 1;
  }
  int32_t qmax() const { return (int32_t)((1u << (cfg_.act_bits - 1)) - 1); }
  int32_t clampQ(int64_t v, int group = -1) {
    int32_t m = qmax();
    if (v > m || v < -m) {
      saturated_++;
      if (group >= 0) groups_[group].saturated++;
      return v > m ? m : -m;
    }
    return (int32_t)v;
  }
  int headDim() const { return 32 / n_heads_; }

  void record(int g, const std::vector<float>& v, bool on) {
    if (!on) return;
    for (float x : v) groups_[g].max = std::max(groups_[g].max, std::fabs(x));
  }

  static std::vector<float> vec(const tinytts::WeightStore& ws, const std::string& name) {
    const tinytts::WeightStore::Entry* e = ws.get(name);
    if (!e) throw std::runtime_error("missing tensor " + name);
    std::vector<float> v(e->count);
    for (size_t i = 0; i < e->count; i++) v[i] = e->at(i);
    return v;
  }
  static std::vector<float> table(const tinytts::WeightStore& ws, const std::string& name) { return vec(ws, name); }
  static Conv linear(const tinytts::WeightStore& ws, const std::string& name, int k) {
    const tinytts::WeightStore::Entry* e = ws.get(name + ".weight");
    if (!e || e->shape.size() != 3 || e->shape[2] != k) throw std::runtime_error("bad weight " + name);
    Conv c;
    c.cout = e->shape[0];
    c.cin = e->shape[1];
    c.k = k;
    c.w.resize(e->count);
    for (int co = 0; co < c.cout; co++)
      for (int ci = 0; ci < c.cin; ci++)
        for (int kk = 0; kk < k; kk++)
          c.w[((size_t)co * k + kk) * c.cin + ci] = e->at(((size_t)co * c.cin + ci) * k + kk);
    c.b = vec(ws, name + ".bias");
    return c;
  }

  template <typename V>
  static void flip(std::vector<V>& x, int T, int C) {
    for (int t = 0; t < T; t++) std::reverse(x.begin() + (size_t)t * C, x.begin() + (size_t)(t + 1) * C);
  }
  template <typename V>
  static std::vector<V> half(const std::vector<V>& x, int T, int off) {
    std::vector<V> h((size_t)T * 16);
    for (int t = 0; t < T; t++)
      for (int c = 0; c < 16; c++) h[(size_t)t * 16 + c] = x[(size_t)t * 32 + off + c];
    return h;
  }
  template <typename V>
  static void join(std::vector<V>& x, const std::vector<V>& x0, const std::vector<V>& x1, int T) {
    for (int t = 0; t < T; t++)
      for (int c = 0; c < 16; c++) {
        x[(size_t)t * 32 + c] = x0[(size_t)t * 16 + c];
        x[(size_t)t * 32 + 16 + c] = x1[(size_t)t * 16 + c];
      }
  }

  // ---------------------------------------------------------------- float
  static std::vector<float> convF(const Conv& c, const std::vector<float>& x, int T) {
    std::vector<float> y((size_t)T * c.cout);
    int pad = (c.k - 1) / 2;
    for (int t = 0; t < T; t++)
      for (int co = 0; co < c.cout; co++) {
        double acc = c.b[co];
        for (int kk = 0; kk < c.k; kk++) {
          int ti = t - pad + kk;
          if (ti < 0 || ti >= T) continue;
          const float* w = c.w.data() + ((size_t)co * c.k + kk) * c.cin;
          const float* xr = x.data() + (size_t)ti * c.cin;
          for (int ci = 0; ci < c.cin; ci++) acc += (double)w[ci] * xr[ci];
        }
        y[(size_t)t * c.cout + co] = (float)acc;
      }
    return y;
  }

  std::vector<float> attentionF(const Layer& L, const std::vector<float>& q, const std::vector<float>& k,
                                const std::vector<float>& v, int T) const {
    int hd = headDim(), w = window_;
    std::vector<float> merged((size_t)T * 32), s(T);
    float scale = 1.0f / std::sqrt((float)hd);
    for (int h = 0; h < n_heads_; h++) {
      int off = h * hd;
      for (int i = 0; i < T; i++) {
        for (int j = 0; j < T; j++) {
          double acc = 0;
          int d = j - i;
          for (int ch = 0; ch < hd; ch++) {
            double kv = k[(size_t)j * 32 + off + ch];
            if (d >= -w && d <= w) kv += L.rel_k[(size_t)(d + w) * hd + ch];
            acc += q[(size_t)i * 32 + off + ch] * kv;
          }
          s[j] = (float)(acc * scale);
        }
        float mx = *std::max_element(s.begin(), s.end());
        double sum = 0;
        for (int j = 0; j < T; j++) {
          s[j] = std::exp(s[j] - mx);
          sum += s[j];
        }
        for (int ch = 0; ch < hd; ch++) {
          double acc = 0;
          for (int j = 0; j < T; j++) {
            double vv = v[(size_t)j * 32 + off + ch];
            int d = j - i;
            if (d >= -w && d <= w) vv += L.rel_v[(size_t)(d + w) * hd + ch];
            acc += s[j] / sum * vv;
          }
          merged[(size_t)i * 32 + off + ch] = (float)acc;
        }
      }
    }
    return merged;
  }

  static void layerNormF(std::vector<float>& x, int T, const std::vector<float>& g, const std::vector<float>& b) {
    int C = (int)g.size();
    for (int t = 0; t < T; t++) {
      float* r = x.data() + (size_t)t * C;
      double mean = 0, var = 0;
      for (int c = 0; c < C; c++) mean += r[c];
      mean /= C;
      for (int c = 0; c < C; c++) var += (r[c] - mean) * (r[c] - mean);
      var /= C;
      double inv = 1.0 / std::sqrt(var + 1e-5);
      for (int c = 0; c < C; c++) r[c] = (float)((r[c] - mean) * inv * g[c] + b[c]);
    }
  }

  // ---------------------------------------------------------------- integer
  static void toMultShiftAny(double m, int32_t& mult, int32_t& shift) {
    if (!(m > 0)) throw std::runtime_error("multiplier must be positive");
    int e;
    std::frexp(m, &e);
    shift = 15 - e;
    mult = (int32_t)std::llround(std::ldexp(m, shift));
    if (mult == 32768) {
      mult = 16384;
      shift--;
    }
  }
  static int64_t shiftRound(int64_t v, int s) { return s > 0 ? roundShift(v, s) : v << (-s); }

  void quantizeConv(Conv& c, int in_g, int out_g) {
    c.in_group = in_g;
    c.out_group = out_g;
    float sx = groups_[in_g].scale, sy = groups_[out_g].scale;
    long wmax = (1L << (cfg_.weight_bits - 1)) - 1;
    size_t row = (size_t)c.k * c.cin;
    c.wq.resize(c.w.size());
    c.bq.resize(c.cout);
    c.mult.resize(c.cout);
    c.shift.resize(c.cout);
    for (int co = 0; co < c.cout; co++) {
      float m = 0;
      for (size_t n = 0; n < row; n++) m = std::max(m, std::fabs(c.w[co * row + n]));
      float ws = m > 0 ? m / (float)wmax : 1.0f;
      for (size_t n = 0; n < row; n++)
        c.wq[co * row + n] = (int32_t)std::clamp<long>(std::lround(c.w[co * row + n] / ws), -wmax, wmax);
      c.bq[co] = std::llround((double)c.b[co] / ((double)ws * sx));
      toMultShiftAny((double)ws * sx / sy, c.mult[co], c.shift[co]);
    }
  }

  std::vector<int32_t> convI(const Conv& c, const std::vector<int32_t>& x, int T, const std::vector<int32_t>* res,
                             bool relu) {
    std::vector<int32_t> y((size_t)T * c.cout);
    int pad = (c.k - 1) / 2;
    for (int t = 0; t < T; t++)
      for (int co = 0; co < c.cout; co++) {
        int64_t acc = c.bq[co];
        for (int kk = 0; kk < c.k; kk++) {
          int ti = t - pad + kk;
          if (ti < 0 || ti >= T) continue;
          const int32_t* w = c.wq.data() + ((size_t)co * c.k + kk) * c.cin;
          const int32_t* xr = x.data() + (size_t)ti * c.cin;
          for (int ci = 0; ci < c.cin; ci++) acc += (int64_t)w[ci] * xr[ci];
        }
        max_acc_ = std::max<int64_t>(max_acc_, acc < 0 ? -acc : acc);
        int64_t v = shiftRound(acc * c.mult[co], c.shift[co]);
        if (relu && v < 0) v = 0;  // before clamping: negatives become 0 either way
        int32_t q = clampQ(v, c.out_group);
        if (res) q = clampQ((int64_t)q + (*res)[(size_t)t * c.cout + co], c.out_group);
        y[(size_t)t * c.cout + co] = q;
      }
    return y;
  }

  void initTables() {
    tables_.act_bits = cfg_.act_bits;
    tables_.exp_bits = cfg_.exp_lut_bits;
    tables_.rsqrt_bits = cfg_.rsqrt_lut_bits;
    tables_.score_frac = cfg_.score_frac;
    tables_.init();
  }

  std::vector<int32_t> attentionI(const Layer& L, const std::vector<int32_t>& q, const std::vector<int32_t>& k,
                                  const std::vector<int32_t>& v, int T) {
    std::vector<int32_t> merged((size_t)T * 32);
    uint64_t sat = 0;
    flowops::attention(q.data(), k.data(), v.data(), T, 32, L.attn, tables_, merged.data(), &sat);
    if (!capture.att_done) {
      capture.att_done = true;
      capture.q = q;
      capture.k = k;
      capture.v = v;
      capture.merged = merged;
      capture.attn = L.attn;
    }
    saturated_ += sat;
    groups_[L.g_m].saturated += sat;
    return merged;
  }

  void quantizeNorm(const std::vector<float>& g, const std::vector<float>& b, int in_g, int out_g, Norm& n) {
    double sx = groups_[in_g].scale, sy = groups_[out_g].scale;
    n.out_group = out_g;
    // W = sum(u^2) / 32 = 1024 * var / s^2 (u = 32 x - sum x): eps in the same units
    n.eps = std::llround(1024.0 * 1e-5 / (sx * sx));
    double gmax = 0;
    for (float v : g) gmax = std::max(gmax, std::fabs((double)v / sy));
    n.gs = 0;
    while (gmax * std::ldexp(1.0, n.gs + 1) < 32767.0) n.gs++;  // gamma: 16-bit signed
    n.g.resize(g.size());
    n.b.resize(b.size());
    for (size_t c = 0; c < g.size(); c++) {
      n.g[c] = std::llround((double)g[c] / sy * std::ldexp(1.0, n.gs));
      n.b[c] = std::llround((double)b[c] / sy);
    }
  }

  void layerNormI(std::vector<int32_t>& x, int T, const Norm& n) {
    uint64_t sat = 0;
    if (!capture.ln_done) capture.ln_in = x;
    for (int t = 0; t < T; t++) flowops::layerNormFrame(x.data() + (size_t)t * 32, n, tables_, &sat);
    if (!capture.ln_done) {
      capture.ln_done = true;
      capture.ln_out = x;
      capture.norm = n;
    }
    saturated_ += sat;
    groups_[n.out_group].saturated += sat;
  }

  FlowConfig cfg_;
  int n_heads_ = 2, window_ = 4;
  std::vector<Group> groups_;
  int z_group_ = 0;
  std::vector<Coupling> couplings_;
  FlowTables tables_;
  uint64_t saturated_ = 0;
  int64_t max_acc_ = 0;
};

}  // namespace tnv
