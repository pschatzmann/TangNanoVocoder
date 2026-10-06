#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "FlowFixed.h"
#include "Program.h"

namespace tnv {

/**
 * @brief The quantized program as the gateware runs it: an SDRAM image of
 * 32-bit words with a header, one 16-word descriptor per op, per-channel
 * parameters and weights, plus the SDRAM layout of every activation buffer.
 * The host uploads it once (link command 'P'); see docs/gateware.md,
 * "Hardware image", for the format.
 *
 * Activations are stored channel-major: channel c, frame t of a buffer is
 * the 16-bit half (t & 1) of word base + c * plane + t / 2. Planes are
 * sized for `max_frames` latent frames and rounded up to SDRAM rows (256
 * words), so a sentence may have at most max_frames frames.
 *
 * Vocoder buffers are shared by liveness, per stage: X (stage input and
 * output, two of them alternating between stages), M (first conv of each
 * residual pair), and a block of four slots R0 R1 R2 A, where R0..R2 are
 * the three resblock outputs and A the residual stream inside a resblock
 * (updated in place: a conv2 reads its residual for a tile while computing
 * it and only then writes that tile). The resblock average becomes a 1x1
 * convolution over the four slots read as one tensor of 4C channels, with
 * weights 1 for channels c, C + c and 2C + c and 0 for A's - so the
 * vocoder needs convolutions only.
 *
 * With a flow (FlowFixed, docs/studies.md, "The flow in fixed point"), its ops come first and
 * work in place on the latent slot: the link then carries z_p instead of z
 * (same format, same scale). Convolutions use the vocoder's descriptor;
 * LayerNorm and attention have their own (op class in descriptor word 0,
 * bits 7:6). The flow's channel flips cost nothing: the halves are read and
 * written through a permutation folded into the projections' weights.
 */
struct HwImage {
  static constexpr uint32_t kMagic = 0x48564E54u;  // "TNVH"
  static constexpr int kHeaderWords = 16;
  static constexpr int kDescWords = 16;
  static constexpr uint32_t kSdramWords = 1u << 21;  // 8MB
  static constexpr int kWeightRam = 8192;   // values, vocoder_core.v
  static constexpr int kOutBuffer = 8192;   // values, vocoder_core.v
  static constexpr int kBankValues = 16 * 1024;

  std::vector<uint32_t> words;  // the image, loaded at SDRAM word 0
  uint32_t used_words = 0;      // whole SDRAM layout, image included
  int max_frames = 0;
  int flow_ops = 0;
};

namespace hw {

inline uint32_t planeWords(int max_frames, int rate) {
  uint32_t w = (uint32_t)((max_frames * rate + 1) / 2);
  return (w + 255) / 256 * 256;
}

inline int log2Exact(int v, const std::string& what) {
  int l = 0;
  while ((1 << l) < v) l++;
  if ((1 << l) != v) throw std::runtime_error(what + " must be a power of two");
  return l;
}

/// SDRAM regions, resolved once the image size is known
enum RegionId { kNone = -1, kLatent = 0, kPcm, kX0, kX1, kM, kR, kFH, kFQ, kFK, kFV, kFM, kFY, kFF, kRegions };

/// a buffer: region + word offset, plane stride, frames per latent frame
struct Ref {
  int region = kNone;
  uint32_t offset = 0;
  uint32_t plane = 0;
  int rate = 1;
};

/// one op, before addresses are known
struct Spec {
  int op_class = 0;  // 0 convolution, 1 LayerNorm, 2 attention
  std::string name;
  // convolution
  bool transposed = false, residual = false, output = false;
  int cin = 0, cout = 0, k = 1, dil = 1, pad = 0, stride_log2 = 0;
  int32_t lr_mul = 32768;
  std::vector<int32_t> w;                         // [co][kk][ci]
  std::vector<int64_t> bias;
  std::vector<int32_t> mult, shift;
  Ref in, out, res, k_in, v_in;                   // k_in / v_in: attention
  // LayerNorm
  NormParams norm;
  // attention
  AttnParams attn;
};

inline uint32_t i16pair(int32_t lo, int32_t hi) { return (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16); }

}  // namespace hw

/// max_tile > 0 caps the output frames per tile (more tiles per op: for
/// testing the scheduler's tile loop on short sentences). `flow`: include
/// the flow's ops (quantized with the vocoder's z scale).
inline HwImage buildHwImage(const Program& p, int max_frames, int max_tile = 0, const FlowFixed* flow = nullptr) {
  using namespace hw;
  HwImage img;
  img.max_frames = max_frames;
  if (max_frames <= 0 || max_frames > 4096) throw std::runtime_error("max_frames out of range");

  std::vector<Spec> specs;
  uint32_t latent_plane = planeWords(max_frames, 1);

  // ---------------------------------------------------------------- flow
  if (flow) {
    const auto& cps = flow->couplings();
    Ref fh{kFH, 0, latent_plane, 1}, fq{kFQ, 0, latent_plane, 1}, fk{kFK, 0, latent_plane, 1},
        fv{kFV, 0, latent_plane, 1}, fm{kFM, 0, latent_plane, 1}, fy{kFY, 0, latent_plane, 1},
        ff{kFF, 0, latent_plane, 1};
    auto convSpec = [&](const FlowFixed::Conv& c, const std::string& name, Ref in, Ref out) {
      Spec s;
      s.name = name;
      s.cin = c.cin;
      s.cout = c.cout;
      s.k = c.k;
      s.pad = (c.k - 1) / 2;
      s.w = c.wq;
      s.bias = c.bq;
      s.mult = c.mult;
      s.shift = c.shift;
      s.in = in;
      s.out = out;
      return s;
    };
    bool reversed = false;  // logical channel c is plane c (false) or 31 - c (true)
    for (int f = (int)cps.size() - 1; f >= 0; f--) {
      reversed = !reversed;  // the flip
      const FlowFixed::Coupling& cp = cps[f];
      std::string cn = "flow.c" + std::to_string(f);
      // x0 = logical 0..15: planes 0..15, or 31..16 when reversed (read as 16..31, weights permuted)
      Ref x0{kLatent, (reversed ? 16u : 0u) * latent_plane, latent_plane, 1};
      Ref x1{kLatent, (reversed ? 0u : 16u) * latent_plane, latent_plane, 1};
      Spec pre = convSpec(cp.pre, cn + ".pre", x0, fh);
      if (reversed)  // input index i (plane 16 + i) is logical channel 15 - i
        for (int co = 0; co < pre.cout; co++)
          std::reverse(pre.w.begin() + (size_t)co * pre.cin, pre.w.begin() + (size_t)(co + 1) * pre.cin);
      specs.push_back(pre);
      for (size_t l = 0; l < cp.layers.size(); l++) {
        const FlowFixed::Layer& L = cp.layers[l];
        std::string ln = cn + ".l" + std::to_string(l);
        specs.push_back(convSpec(L.q, ln + ".q", fh, fq));
        specs.push_back(convSpec(L.k, ln + ".k", fh, fk));
        specs.push_back(convSpec(L.v, ln + ".v", fh, fv));
        Spec at;
        at.op_class = 2;
        at.name = ln + ".attn";
        at.in = fq;
        at.k_in = fk;
        at.v_in = fv;
        at.out = fm;
        at.attn = L.attn;
        specs.push_back(at);
        Spec o = convSpec(L.o, ln + ".o", fm, fy);
        o.residual = true;
        o.res = fh;
        specs.push_back(o);
        Spec n1;
        n1.op_class = 1;
        n1.name = ln + ".ln1";
        n1.in = fy;
        n1.out = fy;
        n1.norm = L.n1;
        specs.push_back(n1);
        specs.push_back(convSpec(L.ff1, ln + ".ff1", fy, ff));
        Spec f2 = convSpec(L.ff2, ln + ".ff2", ff, fy);
        f2.lr_mul = 0;  // ReLU of ff1, applied when ff2 loads its input
        f2.residual = true;
        f2.res = fy;
        specs.push_back(f2);
        Spec n2;
        n2.op_class = 1;
        n2.name = ln + ".ln2";
        n2.in = fy;
        n2.out = fh;
        n2.norm = L.n2;
        specs.push_back(n2);
      }
      Spec post = convSpec(cp.post, cn + ".post", fh, x1);  // x1 + (-m), in place
      post.residual = true;
      post.res = x1;
      if (reversed) {  // output plane j is logical x1 channel 15 - j
        std::vector<int32_t> w(post.w.size());
        size_t row = (size_t)post.k * post.cin;
        for (int j = 0; j < post.cout; j++)
          std::copy(post.w.begin() + (size_t)(15 - j) * row, post.w.begin() + (size_t)(16 - j) * row,
                    w.begin() + (size_t)j * row);
        post.w = w;
        std::reverse(post.bias.begin(), post.bias.end());
        std::reverse(post.mult.begin(), post.mult.end());
        std::reverse(post.shift.begin(), post.shift.end());
      }
      specs.push_back(post);
    }
    if (reversed) throw std::runtime_error("odd number of flow couplings: z would end up reversed");
    img.flow_ops = (int)specs.size();
  }

  // ---------------------------------------------------------------- vocoder
  // regions per buffer; R slots are sized per stage (offset within kR)
  auto region = [&](int buffer) -> Ref {
    const Buffer& b = p.buffers[buffer];
    uint32_t plane = planeWords(max_frames, b.rate);
    uint32_t slot = (uint32_t)b.channels * plane;  // this stage's R slot size
    if (b.name == "z") return {kLatent, 0, latent_plane, 1};
    if (b.name == "pcm") return {kPcm, 0, planeWords(max_frames, 512), 512};
    if (b.name == "pre") return {kX1, 0, plane, b.rate};
    int stage = b.name[1] - '0';
    if (b.name.size() > 3 && (b.name.compare(2, 4, ".ups") == 0 || b.name.compare(2, 4, ".out") == 0))
      return {stage % 2 ? kX1 : kX0, 0, plane, b.rate};
    size_t rbpos = b.name.find(".rb");
    int rb = b.name[rbpos + 3] - '0';
    int pair = b.name[rbpos + 5] - '0';
    if (b.name.find(".conv1") != std::string::npos) return {kM, 0, plane, b.rate};
    if (pair == 2) return {kR, (uint32_t)rb * slot, plane, b.rate};
    return {kR, 3 * slot, plane, b.rate};  // A
  };
  uint32_t x_size = 0;
  for (const Buffer& b : p.buffers) {
    if (b.name == "z" || b.name == "pcm") continue;
    x_size = std::max(x_size, (uint32_t)b.channels * planeWords(max_frames, b.rate));
  }
  for (const Op& op : p.ops) {
    Spec s;
    s.name = op.name;
    bool sum3 = op.kind == OpKind::kSum3;
    s.transposed = op.kind == OpKind::kConvTranspose;
    s.output = op.kind == OpKind::kOutput;
    s.residual = op.residual >= 0;
    s.cin = sum3 ? 4 * op.cout : op.cin;
    s.cout = op.cout;
    s.k = sum3 ? 1 : op.k;
    s.dil = sum3 ? 1 : op.dilation;
    s.pad = sum3 ? 0 : op.padding;
    s.stride_log2 = s.transposed ? log2Exact(op.stride, op.name + " stride") : 0;
    s.lr_mul = sum3 ? 32768 : op.lr_mul;
    s.in = region(op.in);
    s.out = region(op.out);
    if (s.residual) s.res = region(op.residual);
    if (sum3) {
      int c = op.cout;
      for (int co = 0; co < c; co++)
        for (int ci = 0; ci < 4 * c; ci++) s.w.push_back(ci == co || ci == c + co || ci == 2 * c + co ? 1 : 0);
      s.bias.assign(c, 0);
      uint32_t slot = (uint32_t)c * s.in.plane;
      if (s.in.region != kR || s.in.offset != 0 || region(op.in2).offset != slot || region(op.in3).offset != 2 * slot)
        throw std::runtime_error(op.name + ": resblock outputs are not contiguous");
    } else {
      s.w.assign(op.wq.begin(), op.wq.end());
      s.bias.assign(op.bq.begin(), op.bq.end());
    }
    s.mult.assign(op.mult.begin(), op.mult.end());
    s.shift.assign(op.shift.begin(), op.shift.end());
    specs.push_back(s);
  }

  // ---------------------------------------------------------------- image content
  std::vector<uint32_t> params, weights;
  std::vector<uint32_t> param_at(specs.size()), weight_at(specs.size());
  for (size_t i = 0; i < specs.size(); i++) {
    const Spec& s = specs[i];
    param_at[i] = (uint32_t)params.size();
    weight_at[i] = (uint32_t)weights.size();
    if (s.op_class == 0) {
      for (int co = 0; co < s.cout; co++) {
        if (s.bias[co] > INT32_MAX || s.bias[co] < INT32_MIN)
          throw std::runtime_error(s.name + ": bias exceeds 32 bits");
        if (s.shift[co] < 15 || s.shift[co] > 33)
          throw std::runtime_error(s.name + ": requantize shift outside 15..33 (vocoder_post.v)");
        params.push_back((uint32_t)(int32_t)s.bias[co]);
        params.push_back((uint32_t)s.mult[co] | ((uint32_t)s.shift[co] << 16));
      }
      std::vector<int32_t> w = s.w;
      if (w.size() % 2) w.push_back(0);
      for (size_t n = 0; n < w.size(); n += 2) weights.push_back(i16pair(w[n], w[n + 1]));
    } else if (s.op_class == 1) {
      for (size_t c = 0; c < s.norm.g.size(); c++) {
        if (s.norm.g[c] > 32767 || s.norm.g[c] < -32768 || s.norm.b[c] > INT32_MAX || s.norm.b[c] < INT32_MIN)
          throw std::runtime_error(s.name + ": LayerNorm parameters out of range");
        params.push_back((uint32_t)(int32_t)s.norm.g[c]);
        params.push_back((uint32_t)(int32_t)s.norm.b[c]);
      }
    } else {  // attention: relative tables in the weights area, k then v
      const AttnParams& a = s.attn;
      for (size_t n = 0; n < a.rel_k.size(); n += 2) weights.push_back(i16pair(a.rel_k[n], a.rel_k[n + 1]));
      for (size_t n = 0; n < a.rel_v.size(); n += 2) weights.push_back(i16pair(a.rel_v[n], a.rel_v[n + 1]));
    }
  }
  uint32_t op_table = HwImage::kHeaderWords;
  uint32_t params_base = op_table + (uint32_t)specs.size() * HwImage::kDescWords;
  uint32_t weights_base = params_base + (uint32_t)params.size();
  uint32_t image_words = weights_base + (uint32_t)weights.size();

  // ---------------------------------------------------------------- SDRAM layout
  uint32_t next = (image_words + 255) / 256 * 256;
  auto alloc = [&](uint32_t size) {
    uint32_t a = next;
    next += (size + 255) / 256 * 256;
    return a;
  };
  uint32_t base[kRegions] = {};
  uint32_t latent_base[2], pcm_base[2];
  for (auto& b : latent_base) b = alloc(32 * latent_plane);
  uint32_t pcm_plane = planeWords(max_frames, 512);
  for (auto& b : pcm_base) b = alloc(pcm_plane);
  base[kLatent] = latent_base[0];
  base[kPcm] = pcm_base[0];
  base[kX0] = alloc(x_size);
  base[kX1] = alloc(x_size);
  base[kM] = alloc(x_size);
  base[kR] = alloc(4 * x_size);  // R0 R1 R2 A, slots sized per stage
  if (flow) {
    base[kFH] = alloc(32 * latent_plane);
    base[kFQ] = alloc(32 * latent_plane);
    base[kFK] = alloc(32 * latent_plane);
    base[kFV] = alloc(32 * latent_plane);
    base[kFM] = alloc(32 * latent_plane);
    base[kFY] = alloc(32 * latent_plane);
    base[kFF] = alloc(128 * latent_plane);
  }
  img.used_words = next;
  if (next > HwImage::kSdramWords)
    throw std::runtime_error("SDRAM layout needs " + std::to_string(next) + " words; lower max_frames");
  auto addr = [&](const Ref& r) -> uint32_t { return r.region == kNone ? 0 : base[r.region] + r.offset; };

  // ---------------------------------------------------------------- header and descriptors
  std::vector<uint32_t>& out = img.words;
  out.assign(image_words, 0u);
  float zs = p.groups[p.buffers[p.input].group].scale;
  uint32_t zbits;
  std::memcpy(&zbits, &zs, 4);
  uint32_t flow_info = 0;
  if (flow) {
    const FlowTables& t = flow->tables();
    flow_info = 1u | ((uint32_t)t.exp_bits << 8) | ((uint32_t)t.rsqrt_bits << 16) | ((uint32_t)t.score_frac << 24);
  }
  uint32_t header[HwImage::kHeaderWords] = {HwImage::kMagic, 1, (uint32_t)specs.size(), op_table,
                                            (uint32_t)max_frames, latent_base[0], latent_base[1], latent_plane,
                                            pcm_base[0], pcm_base[1], image_words, zbits, img.used_words,
                                            flow_info, (uint32_t)img.flow_ops, 0};
  std::copy(header, header + HwImage::kHeaderWords, out.begin());

  for (size_t i = 0; i < specs.size(); i++) {
    const Spec& s = specs[i];
    uint32_t* w = &out[op_table + i * HwImage::kDescWords];
    w[2] = addr(s.in);
    w[3] = s.in.plane;
    w[4] = addr(s.out);
    w[5] = s.out.plane;
    w[9] = params_base + param_at[i];
    w[10] = (uint32_t)s.in.rate | ((uint32_t)s.out.rate << 16);
    if (s.op_class == 1) {  // LayerNorm
      if (s.norm.eps > UINT32_MAX) throw std::runtime_error(s.name + ": LayerNorm eps exceeds 32 bits");
      if (s.norm.gs < 0 || s.norm.gs > 30) throw std::runtime_error(s.name + ": LayerNorm gs outside 0..30");
      w[0] = 1u << 6;
      w[1] = 32u | ((uint32_t)s.norm.gs << 8);
      w[11] = (uint32_t)s.norm.eps;
      continue;
    }
    if (s.op_class == 2) {  // attention
      const AttnParams& a = s.attn;
      if (a.score_shift < 1 || a.score_shift > 47 || a.merge_shift < 1 || a.merge_shift > 47)
        throw std::runtime_error(s.name + ": attention shift outside 1..47 (vocoder_flow_unit.v)");
      w[0] = 2u << 6;
      w[1] = (uint32_t)a.heads | ((uint32_t)a.head_dim << 8) | ((uint32_t)a.window << 16);
      w[6] = addr(s.k_in);
      w[7] = addr(s.v_in);
      w[8] = weights_base + weight_at[i];
      w[11] = (uint32_t)a.score_mult | ((uint32_t)(uint8_t)(int8_t)a.score_shift << 16);
      w[12] = (uint32_t)a.merge_mult | ((uint32_t)(uint8_t)(int8_t)a.merge_shift << 16);
      continue;
    }
    // convolution
    int cin_log2 = log2Exact(s.cin, s.name + " cin");
    int a = s.transposed ? s.pad - (s.k - 1) : -s.pad;
    int b = s.transposed ? s.pad : (s.k - 1) * s.dil - s.pad;
    int row = s.k * s.cin;
    int co_group = std::min(s.cout, HwImage::kWeightRam / row);
    if (co_group < 1) throw std::runtime_error(s.name + ": one output channel's weights exceed the weight RAM");
    int unit = 16 << s.stride_log2;
    int max_in_frames = 16 * (1024 / s.cin);
    int n = (HwImage::kOutBuffer / co_group) / unit * unit;
    if (max_tile > 0) n = std::max(unit, std::min(n, max_tile / unit * unit));
    auto in_frames = [&](int nn) { return ((nn - 1 + b - a) >> s.stride_log2) + 2; };
    while (n > unit && in_frames(n) > max_in_frames) n -= unit;
    if (n < 16 || in_frames(n) > max_in_frames || (long)n * co_group > HwImage::kOutBuffer)
      throw std::runtime_error(s.name + ": no tile size fits");
    bool in_latent = s.in.region == kLatent, out_pcm = s.out.region == kPcm, out_latent = s.out.region == kLatent;
    w[0] = (s.transposed ? 1u : 0u) | (s.residual ? 1u << 2 : 0u) | (s.output ? 1u << 3 : 0u) |
           (in_latent ? 1u << 4 : 0u) | (out_pcm ? 1u << 5 : 0u) | ((uint32_t)cin_log2 << 8) |
           ((uint32_t)s.stride_log2 << 12) | ((uint32_t)s.k << 16) | ((uint32_t)s.dil << 24) |
           (out_latent ? 1u << 28 : 0u) | (s.residual && s.res.region == kLatent ? 1u << 29 : 0u);
    w[1] = (uint32_t)s.cout | ((uint32_t)s.pad << 8) | ((uint32_t)s.lr_mul << 16);
    w[6] = s.residual ? addr(s.res) : 0;
    w[7] = s.residual ? s.res.plane : 0;
    w[8] = weights_base + weight_at[i];
    w[11] = (uint32_t)co_group | ((uint32_t)n << 16);
    w[12] = (uint32_t)(uint16_t)(int16_t)a | ((uint32_t)(uint16_t)(int16_t)b << 16);
  }
  std::copy(params.begin(), params.end(), out.begin() + params_base);
  std::copy(weights.begin(), weights.end(), out.begin() + weights_base);
  return img;
}

}  // namespace tnv
