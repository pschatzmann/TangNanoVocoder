#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

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
 * Buffers are shared by liveness, per stage: X (stage input and output,
 * two of them alternating between stages), M (first conv of each residual
 * pair), and a block of four slots R0 R1 R2 A, where R0..R2 are the three
 * resblock outputs and A the residual stream inside a resblock (updated in
 * place: a conv2 reads its residual for a tile while computing it and only
 * then writes that tile). The resblock average becomes a 1x1 convolution
 * over the four slots read as one tensor of 4C channels, with weights 1 for
 * channels c, C + c and 2C + c and 0 for A's - so the gateware only knows
 * convolutions.
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
};

namespace hw {

struct Region {
  uint32_t base = 0, plane = 0;
};

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

}  // namespace hw

/// max_tile > 0 caps the output frames per tile (more tiles per op: for
/// testing the scheduler's tile loop on short sentences).
inline HwImage buildHwImage(const Program& p, int max_frames, int max_tile = 0) {
  using hw::Region;
  HwImage img;
  img.max_frames = max_frames;
  if (max_frames <= 0 || max_frames > 4096) throw std::runtime_error("max_frames out of range");

  // ---- descriptors to emit: every op, sum3 rewritten as a 1x1 conv
  struct Desc {
    const Op* op;
    bool sum3;
    int stage;     // -1: conv_pre / conv_post
    int rb, pair;  // resblock and residual pair, for convs inside a stage
    bool conv2;
  };
  std::vector<Desc> descs;
  for (const Op& op : p.ops) {
    Desc d{&op, op.kind == OpKind::kSum3, -1, -1, -1, false};
    if (op.name[0] == 's' && op.name[1] >= '0' && op.name[1] <= '9') {
      d.stage = op.name[1] - '0';
      size_t rbpos = op.name.find(".rb");
      if (rbpos != std::string::npos) {
        d.rb = op.name[rbpos + 3] - '0';
        d.pair = op.name[rbpos + 5] - '0';
        d.conv2 = op.name.find(".conv2") != std::string::npos;
      }
    }
    descs.push_back(d);
  }

  // ---- SDRAM layout: image, latent slots, PCM slots, activation regions
  // Sizes of the per-stage regions: the largest over the stages.
  uint32_t x_size = 0, m_size = 0, r_slot = 0;
  for (const Buffer& b : p.buffers) {
    uint32_t size = (uint32_t)b.channels * hw::planeWords(max_frames, b.rate);
    if (b.name == "z" || b.name == "pcm") continue;
    x_size = std::max(x_size, size);  // X holds conv_pre, ups outputs, stage outputs
    m_size = std::max(m_size, size);
    r_slot = std::max(r_slot, size);
  }

  // image content first, to know its size
  std::vector<uint32_t> params, weights;
  std::vector<uint32_t> param_at(descs.size()), weight_at(descs.size());
  for (size_t i = 0; i < descs.size(); i++) {
    const Op& op = *descs[i].op;
    param_at[i] = (uint32_t)params.size();
    weight_at[i] = (uint32_t)weights.size();
    std::vector<int32_t> w;  // [co][kk][ci]
    if (descs[i].sum3) {
      int c = op.cout;
      for (int co = 0; co < c; co++)
        for (int ci = 0; ci < 4 * c; ci++) w.push_back(ci == co || ci == c + co || ci == 2 * c + co ? 1 : 0);
      for (int co = 0; co < c; co++) {
        params.push_back(0u);
        params.push_back((uint32_t)op.mult[co] | ((uint32_t)op.shift[co] << 16));
      }
    } else {
      w.assign(op.wq.begin(), op.wq.end());
      for (int co = 0; co < op.cout; co++) {
        params.push_back((uint32_t)op.bq[co]);
        params.push_back((uint32_t)op.mult[co] | ((uint32_t)op.shift[co] << 16));
      }
    }
    if (w.size() % 2) w.push_back(0);
    for (size_t n = 0; n < w.size(); n += 2)
      weights.push_back((uint32_t)(uint16_t)w[n] | ((uint32_t)(uint16_t)w[n + 1] << 16));
  }
  uint32_t op_table = HwImage::kHeaderWords;
  uint32_t params_base = op_table + (uint32_t)descs.size() * HwImage::kDescWords;
  uint32_t weights_base = params_base + (uint32_t)params.size();
  uint32_t image_words = weights_base + (uint32_t)weights.size();

  uint32_t next = (image_words + 255) / 256 * 256;
  auto alloc = [&](uint32_t size) {
    uint32_t a = next;
    next += (size + 255) / 256 * 256;
    return a;
  };
  uint32_t latent_plane = hw::planeWords(max_frames, 1);
  uint32_t latent_base[2], pcm_base[2];
  for (auto& b : latent_base) b = alloc(32 * latent_plane);
  uint32_t pcm_plane = hw::planeWords(max_frames, 512);
  for (auto& b : pcm_base) b = alloc(pcm_plane);
  uint32_t x_base[2] = {alloc(x_size), alloc(x_size)};
  uint32_t m_base = alloc(m_size);
  uint32_t r_base = alloc(4 * r_slot);  // R0 R1 R2 A, slots sized per stage below
  img.used_words = next;
  if (next > HwImage::kSdramWords)
    throw std::runtime_error("SDRAM layout needs " + std::to_string(next) + " words; lower max_frames");

  // where each buffer lives
  auto region = [&](int buffer) -> Region {
    const Buffer& b = p.buffers[buffer];
    uint32_t plane = hw::planeWords(max_frames, b.rate);
    uint32_t slot = (uint32_t)b.channels * plane;  // this stage's R slot size
    if (b.name == "z") return {latent_base[0], latent_plane};
    if (b.name == "pcm") return {pcm_base[0], pcm_plane};
    if (b.name == "pre") return {x_base[1], plane};
    int stage = b.name[1] - '0';
    if (b.name.size() > 3 && b.name.compare(2, 4, ".ups") == 0) return {x_base[stage % 2], plane};
    if (b.name.size() > 3 && b.name.compare(2, 4, ".out") == 0) return {x_base[stage % 2], plane};
    size_t rbpos = b.name.find(".rb");
    int rb = b.name[rbpos + 3] - '0';
    int pair = b.name[rbpos + 5] - '0';
    if (b.name.find(".conv1") != std::string::npos) return {m_base, plane};
    if (pair == 2) return {r_base + (uint32_t)rb * slot, plane};
    return {r_base + 3 * slot, plane};  // A
  };

  // ---- header and descriptors
  std::vector<uint32_t>& out = img.words;
  out.assign(image_words, 0u);
  float zs = p.groups[p.buffers[p.input].group].scale;
  uint32_t zbits;
  std::memcpy(&zbits, &zs, 4);
  uint32_t header[HwImage::kHeaderWords] = {HwImage::kMagic, 1, (uint32_t)descs.size(), op_table,
                                            (uint32_t)max_frames, latent_base[0], latent_base[1], latent_plane,
                                            pcm_base[0], pcm_base[1], image_words, zbits, img.used_words, 0, 0, 0};
  std::copy(header, header + HwImage::kHeaderWords, out.begin());

  for (size_t i = 0; i < descs.size(); i++) {
    const Desc& d = descs[i];
    const Op& op = *d.op;
    bool transposed = op.kind == OpKind::kConvTranspose;
    int cin = d.sum3 ? 4 * op.cout : op.cin;
    int k = d.sum3 ? 1 : op.k, dil = d.sum3 ? 1 : op.dilation, pad = d.sum3 ? 0 : op.padding;
    int stride_log2 = transposed ? hw::log2Exact(op.stride, op.name + " stride") : 0;
    int cin_log2 = hw::log2Exact(cin, op.name + " cin");
    const Buffer& bin = p.buffers[op.in];
    const Buffer& bout = p.buffers[op.out];
    Region rin = region(op.in), rout = region(op.out);
    Region rres;
    if (op.residual >= 0) rres = region(op.residual);

    // halo: input frames lo = (t0 + a) >> s, hi = (t0 + n - 1 + b) >> s
    int a = transposed ? pad - (k - 1) : -pad;
    int b = transposed ? pad : (k - 1) * dil - pad;

    // co groups: weights of a group fit the weight RAM
    int row = k * cin;
    int co_group = std::min(op.cout, HwImage::kWeightRam / row);
    if (co_group < 1) throw std::runtime_error(op.name + ": one output channel's weights exceed the weight RAM");
    // tile: outputs per run, multiple of 16 (x stride for transposed),
    // output tile fits the buffer, input tile (with halo) fits the banks
    int unit = 16 << stride_log2;
    int max_in_frames = 16 * (1024 / cin);
    int n = (HwImage::kOutBuffer / co_group) / unit * unit;
    if (max_tile > 0) n = std::max(unit, std::min(n, max_tile / unit * unit));
    auto in_frames = [&](int nn) { return ((nn - 1 + b - a) >> stride_log2) + 2; };
    while (n > unit && in_frames(n) > max_in_frames) n -= unit;
    if (n < 16 || in_frames(n) > max_in_frames || (long)n * co_group > HwImage::kOutBuffer)
      throw std::runtime_error(op.name + ": no tile size fits");

    for (int32_t sh : op.shift)
      if (sh < 15 || sh > 32) throw std::runtime_error(op.name + ": requantize shift outside 15..32 (vocoder_post.v)");

    uint32_t* w = &out[op_table + i * HwImage::kDescWords];
    bool in_latent = bin.name == "z", out_pcm = bout.name == "pcm";
    w[0] = (transposed ? 1u : 0u) | (op.residual >= 0 ? 1u << 2 : 0u) | (op.kind == OpKind::kOutput ? 1u << 3 : 0u) |
           (in_latent ? 1u << 4 : 0u) | (out_pcm ? 1u << 5 : 0u) | ((uint32_t)cin_log2 << 8) |
           ((uint32_t)stride_log2 << 12) | ((uint32_t)k << 16) | ((uint32_t)dil << 24);
    w[1] = (uint32_t)op.cout | ((uint32_t)pad << 8) | ((uint32_t)(d.sum3 ? 32768 : op.lr_mul) << 16);
    w[2] = rin.base;
    w[3] = rin.plane;
    w[4] = rout.base;
    w[5] = rout.plane;
    w[6] = rres.base;
    w[7] = rres.plane;
    w[8] = weights_base + weight_at[i];
    w[9] = params_base + param_at[i];
    w[10] = (uint32_t)bin.rate | ((uint32_t)bout.rate << 16);
    w[11] = (uint32_t)co_group | ((uint32_t)n << 16);
    w[12] = (uint32_t)(uint16_t)(int16_t)a | ((uint32_t)(uint16_t)(int16_t)b << 16);
    if (d.sum3) {
      // the sum reads R0..R2 (+ A), all of this stage's slot size: check
      // they're one contiguous tensor of 4C channels
      uint32_t slot = (uint32_t)op.cout * rin.plane;
      if (rin.base != r_base || region(op.in2).base != r_base + slot || region(op.in3).base != r_base + 2 * slot)
        throw std::runtime_error(op.name + ": resblock outputs are not contiguous");
    }
  }
  std::copy(params.begin(), params.end(), out.begin() + params_base);
  std::copy(weights.begin(), weights.end(), out.begin() + weights_base);
  return img;
}

}  // namespace tnv
