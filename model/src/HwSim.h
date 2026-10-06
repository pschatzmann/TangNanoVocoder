#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "Executors.h"
#include "HwImage.h"

namespace tnv {

/**
 * @brief Runs a hardware image the way the gateware's scheduler does
 * (vocoder_core.v): op by op from the descriptors, output channels in
 * weight-RAM groups, output frames in tiles, each tile's input loaded with
 * its halo, residuals read while a tile is computed and the tile written
 * only afterwards, every value in channel-major packed SDRAM. It is the
 * executable spec of the scheduler: its PCM must equal the model's.
 */
class HwSim {
 public:
  explicit HwSim(const std::vector<uint32_t>& image) : mem_(HwImage::kSdramWords, 0u) {
    if (image.size() < (size_t)HwImage::kHeaderWords || image[0] != HwImage::kMagic)
      throw std::runtime_error("not a TNVH image");
    std::copy(image.begin(), image.end(), mem_.begin());
  }

  /// Estimated clock cycles of the last run(), following vocoder_core.v's
  /// sequence: every SDRAM burst as its length plus kBurstOverhead (request,
  /// activate, CAS latency, precharge, arbiter), the engine as max(k * cin,
  /// 16) cycles per block of 16 outputs (a block's results drain one per
  /// clock), plus refresh. Calibrated against the system simulation; the
  /// board's own count is the 'T' reply.
  struct Cycles {
    double total = 0, engine = 0, dma = 0, fixed = 0;
  };
  const Cycles& cycles() const { return cyc_; }
  static constexpr int kBurstOverhead = 13;

  /// zq: quantized latent [frames][32], as the link delivers it.
  std::vector<int16_t> run(const std::vector<int32_t>& zq, int frames) {
    cyc_ = Cycles();
    const uint32_t* h = mem_.data();
    if (frames > (int)h[4]) throw std::runtime_error("sentence longer than the image's max_frames");
    uint32_t latent = h[5], latent_plane = h[7], pcm = h[8];
    for (int t = 0; t < frames; t++)
      for (int c = 0; c < 32; c++) put(latent + c * latent_plane, t, zq[(size_t)t * 32 + c]);

    for (uint32_t i = 0; i < h[2]; i++) runOp(&mem_[h[3] + i * HwImage::kDescWords], frames);
    cyc_.total = (cyc_.engine + cyc_.dma + cyc_.fixed) * 1.02;  // refresh: one AUTO REFRESH per 7us

    std::vector<int16_t> out((size_t)frames * 512);
    for (size_t t = 0; t < out.size(); t++) out[t] = get(pcm, (int)t);
    return out;
  }

 private:
  int16_t get(uint32_t plane_base, int t) const {
    uint32_t w = mem_.at(plane_base + (uint32_t)t / 2);
    return (int16_t)(t & 1 ? w >> 16 : w & 0xFFFF);
  }
  void put(uint32_t plane_base, int t, int32_t v) {
    uint32_t& w = mem_.at(plane_base + (uint32_t)t / 2);
    if (t & 1) w = (w & 0xFFFFu) | ((uint32_t)(uint16_t)v << 16);
    else w = (w & 0xFFFF0000u) | (uint16_t)v;
  }
  int16_t weight(uint32_t base, uint32_t n) const {
    uint32_t w = mem_.at(base + n / 2);
    return (int16_t)(n & 1 ? w >> 16 : w & 0xFFFF);
  }

  /// one DMA transfer: `segs` segments of `words`, `stride` apart, row-limited bursts
  double dmaCycles(uint32_t first, uint32_t stride, int segs, uint32_t words, uint32_t max_burst = 256) const {
    double c = 0;
    for (int sgm = 0; sgm < segs; sgm++) {
      uint32_t a = first + (uint32_t)sgm * stride, left = words;
      while (left) {
        uint32_t len = std::min({left, 256 - (a & 255), max_burst});
        c += len + kBurstOverhead;
        a += len;
        left -= len;
      }
    }
    return c;
  }

  void runOp(const uint32_t* d, int frames) {
    bool transposed = d[0] & 1, residual = d[0] >> 2 & 1, output = d[0] >> 3 & 1;
    int cin = 1 << (d[0] >> 8 & 7), s = d[0] >> 12 & 7, k = d[0] >> 16 & 31, dil = d[0] >> 24 & 7;
    int cout = d[1] & 0xFF, pad = d[1] >> 8 & 63;
    int32_t lr_mul = (int32_t)(d[1] >> 16);
    uint32_t in_base = d[2], in_plane = d[3], out_base = d[4], out_plane = d[5], res_base = d[6], res_plane = d[7];
    uint32_t wbase = d[8], pbase = d[9];
    int tin = frames * (int)(d[10] & 0xFFFF), tout = frames * (int)(d[10] >> 16);
    int group = (int)(d[11] & 0xFFFF), tile = (int)(d[11] >> 16);
    int a = (int16_t)(d[12] & 0xFFFF), b = (int16_t)(d[12] >> 16);
    int row = k * cin;

    std::vector<int32_t> x;  // tile [frame - lo][ci]
    std::vector<int16_t> obuf((size_t)group * tile);
    cyc_.dma += dmaCycles(0, 0, 1, 16) + dmaCycles(pbase, 0, 1, (uint32_t)cout * 2);
    cyc_.fixed += 10;
    uint32_t wnext = wbase;
    for (int co0 = 0; co0 < cout; co0 += group) {
      int cnt = std::min(group, cout - co0);
      uint32_t wwords = (uint32_t)(cnt * row / 2);
      cyc_.dma += dmaCycles(wnext, 0, 1, wwords);
      wnext += wwords;
      for (int t0 = 0; t0 < tout; t0 += tile) {
        int n = std::min(tile, tout - t0);
        int lo = std::max(0, (t0 + a) >> s), hi = std::min(tin - 1, (t0 + n - 1 + b) >> s);
        int frames_in = hi - lo + 1;
        cyc_.dma += dmaCycles(in_base + (uint32_t)(lo >> 1), in_plane, cin, (uint32_t)((hi >> 1) - (lo >> 1) + 1));
        cyc_.dma += dmaCycles(out_base + (uint32_t)co0 * out_plane + (uint32_t)(t0 >> 1), out_plane, cnt,
                              (uint32_t)((n + 1) / 2));
        cyc_.fixed += 25;  // state changes, engine and post pipelines
        {
          // engine: blocks of 16 outputs, each max(L, 16) cycles
          int taps = transposed ? (k >> s) : k;
          double per_block = std::max(taps * cin, 16);
          double blocks = 0;
          if (transposed) {
            for (int r = 0; r < (1 << s); r++) blocks += std::ceil((double)n / (1 << s) / 16.0);
          } else {
            blocks = std::ceil(n / 16.0);
          }
          cyc_.engine += cnt * blocks * per_block;
        }
        if ((frames_in + 15) / 16 * cin > 1024) throw std::runtime_error("input tile exceeds the banks");
        x.assign((size_t)frames_in * cin, 0);
        for (int ci = 0; ci < cin; ci++)
          for (int f = lo; f <= hi; f++)
            x[(size_t)(f - lo) * cin + ci] = leakyInt(get(in_base + ci * in_plane, f), lr_mul);
        auto in = [&](int f, int ci) -> int32_t {
          if (f < 0 || f >= tin) return 0;
          if (f < lo || f > hi) throw std::runtime_error("frame outside the input tile");
          return x[(size_t)(f - lo) * cin + ci];
        };

        for (int c = 0; c < cnt; c++) {
          int co = co0 + c;
          int32_t bias = (int32_t)mem_[pbase + 2 * co];
          int32_t mult = (int32_t)(mem_[pbase + 2 * co + 1] & 0xFFFF), shift = (int32_t)(mem_[pbase + 2 * co + 1] >> 16);
          for (int t = t0; t < t0 + n; t++) {
            int64_t acc = 0;
            uint32_t wrow = (uint32_t)(c * row);  // weights of the group start at co0
            if (transposed) {
              int base = t + pad;
              for (int kk = base % (1 << s); kk < k; kk += 1 << s)
                for (int ci = 0; ci < cin; ci++)
                  acc += (int64_t)weight(wbase + (uint32_t)(co0 * row) / 2, wrow + kk * cin + ci) *
                         in((base - kk) >> s, ci);
            } else {
              for (int kk = 0; kk < k; kk++)
                for (int ci = 0; ci < cin; ci++)
                  acc += (int64_t)weight(wbase + (uint32_t)(co0 * row) / 2, wrow + kk * cin + ci) *
                         in(t - pad + kk * dil, ci);
            }
            int64_t v = roundShift((acc + bias) * mult, shift);
            uint64_t sat = 0;
            int32_t y;
            if (output) {
              y = tanhQ12ToQ15(clampQ(v, 32767, sat));
            } else {
              y = clampQ(v, 32767, sat);
              if (residual) y = clampQ((int64_t)y + get(res_base + co * res_plane, t), 32767, sat);
            }
            obuf[(size_t)c * tile + (t - t0)] = (int16_t)y;
          }
        }
        for (int c = 0; c < cnt; c++)
          for (int t = t0; t < t0 + n; t++) put(out_base + (co0 + c) * out_plane, t, obuf[(size_t)c * tile + (t - t0)]);
      }
    }
  }

  std::vector<uint32_t> mem_;
  Cycles cyc_;
};

}  // namespace tnv
