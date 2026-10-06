#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "TinyTTS/WeightStore.h"

namespace tnv {

/// What one step of the layer program does. The FPGA's layer scheduler runs
/// the same list (see docs/studies.md).
enum class OpKind : uint8_t {
  kConv = 0,           ///< Conv1d, "same" padding, stride 1, optional dilation
  kConvTranspose = 1,  ///< ConvTranspose1d (upsampling), computed as a gather
  kSum3 = 2,           ///< (in + in2 + in3) / 3: the stage's resblock average
  kOutput = 3,         ///< conv_post, then tanh -> 16-bit PCM
};

/// One activation tensor, [frames][channels], frames = latent frames * rate.
struct Buffer {
  std::string name;
  int channels = 0;
  int rate = 1;   ///< frames per latent frame
  int group = 0;  ///< index into Program::groups: buffers in a group share one scale
};

/// Buffers that must share a scale (residual adds work on raw integers), or
/// that are fixed by the format (z on the link, PCM out).
struct ScaleGroup {
  std::string name;
  int bits = 8;         ///< signed width; values are clamped to +-(2^(bits-1)-1)
  float scale = 1.0f;   ///< real value = integer * scale
  bool fixed = false;   ///< not set by calibration

  int32_t qmax() const { return (int32_t)((1u << (bits - 1)) - 1); }
};

struct Op {
  OpKind kind = OpKind::kConv;
  std::string name;
  int in = -1, out = -1;
  int residual = -1;       ///< kConv: buffer added to the result (same group as out)
  int in2 = -1, in3 = -1;  ///< kSum3
  int cin = 0, cout = 0, k = 1, dilation = 1, stride = 1, padding = 0;
  bool leaky = false;      ///< leaky ReLU on the input before the convolution
  float slope = 0.0f;

  // Float weights in gather layout [cout][k][cin] and bias [cout].
  std::vector<float> w;
  std::vector<float> bias;

  // Set by quantize() (Quantize.h).
  int wbits = 8;               ///< signed weight width
  std::vector<int16_t> wq;     ///< [cout][k][cin], within +-(2^(wbits-1)-1)
  std::vector<float> wscale;   ///< per output channel
  std::vector<int32_t> bq;     ///< bias in accumulator units
  std::vector<int32_t> mult;   ///< requantize multiplier per output channel, in [2^14, 2^15)
  std::vector<int32_t> shift;  ///< requantize right shift per output channel
  int32_t lr_mul = 32768;      ///< leaky ReLU slope for negative inputs, Q15 (32768 = identity)
};

struct Program {
  std::vector<ScaleGroup> groups;
  std::vector<Buffer> buffers;
  std::vector<Op> ops;
  int input = -1;   ///< buffer: latent z [T][32]
  int output = -1;  ///< buffer: PCM [T*512][1]
};

/// Scale of conv_post's result before tanh: Q12, so +-8.0 fits 16 bits
/// (tanh(8) rounds to 1.0 at 16-bit precision).
constexpr int kTanhInFracBits = 12;

namespace detail {

inline int addGroup(Program& p, const std::string& name, int bits, float scale = 1.0f, bool fixed = false) {
  p.groups.push_back({name, bits, scale, fixed});
  return (int)p.groups.size() - 1;
}

inline int addBuffer(Program& p, const std::string& name, int channels, int rate, int group) {
  p.buffers.push_back({name, channels, rate, group});
  return (int)p.buffers.size() - 1;
}

inline std::vector<float> vecOf(const tinytts::WeightStore& ws, const std::string& name) {
  std::vector<float> v = ws.vec(name);
  if (v.empty()) throw std::runtime_error("missing tensor " + name);
  return v;
}

inline const tinytts::WeightStore::Entry& entryOf(const tinytts::WeightStore& ws, const std::string& name) {
  const tinytts::WeightStore::Entry* e = ws.get(name);
  if (!e || e->shape.size() != 3) throw std::runtime_error("missing conv weight " + name);
  return *e;
}

/// Conv1d weight [Cout, Cin, K] -> gather layout [cout][k][cin].
inline void convWeights(const tinytts::WeightStore& ws, const std::string& name, Op& op) {
  const auto& e = entryOf(ws, name);
  op.cout = e.shape[0];
  op.cin = e.shape[1];
  op.k = e.shape[2];
  op.w.resize(e.count);
  for (int co = 0; co < op.cout; co++)
    for (int ci = 0; ci < op.cin; ci++)
      for (int kk = 0; kk < op.k; kk++)
        op.w[((size_t)co * op.k + kk) * op.cin + ci] = e.at(((size_t)co * op.cin + ci) * op.k + kk);
}

/// ConvTranspose1d weight [Cin, Cout, K] -> gather layout [cout][k][cin].
inline void convTransposeWeights(const tinytts::WeightStore& ws, const std::string& name, Op& op) {
  const auto& e = entryOf(ws, name);
  op.cin = e.shape[0];
  op.cout = e.shape[1];
  op.k = e.shape[2];
  op.w.resize(e.count);
  for (int ci = 0; ci < op.cin; ci++)
    for (int co = 0; co < op.cout; co++)
      for (int kk = 0; kk < op.k; kk++)
        op.w[((size_t)co * op.k + kk) * op.cin + ci] = e.at(((size_t)ci * op.cout + co) * op.k + kk);
}

}  // namespace detail

/**
 * @brief Builds the layer program for TinyTTS's vocoder (tinytts::Vocoder,
 * `dec.*` weights), with the speaker conditioning for embedding `g` folded
 * into conv_pre's bias -- it is a constant per speaker.
 *
 * Scale groups: the latent z (link format, `z_bits`), conv_pre's output, per
 * stage one group for the residual stream (upsampling output and every
 * resblock step, so the adds are plain integer adds), one per first conv of
 * each residual pair, one for the stage output, and PCM.
 */
inline Program buildProgram(const tinytts::WeightStore& ws, const std::vector<float>& g, int act_bits, int z_bits,
                            const std::string& prefix = "dec") {
  using namespace detail;
  static const int kRates[5] = {8, 8, 2, 2, 2};
  static const int kUpKernels[5] = {16, 16, 8, 2, 2};
  static const int kResKernels[3] = {3, 7, 11};
  static const int kDilations[3] = {1, 3, 5};

  Program p;
  p.input = addBuffer(p, "z", 32, 1, addGroup(p, "z", z_bits));

  // conv_pre, speaker conditioning folded into the bias.
  Op pre;
  pre.kind = OpKind::kConv;
  pre.name = "conv_pre";
  convWeights(ws, prefix + ".conv_pre.weight", pre);
  pre.padding = (pre.k - 1) / 2;
  pre.bias = vecOf(ws, prefix + ".conv_pre.bias");
  tinytts::Mat cond_w = ws.linearWeight(prefix + ".cond.weight");  // [64, 128]
  std::vector<float> cond_b = vecOf(ws, prefix + ".cond.bias");
  if (cond_w.rows() != pre.cout || cond_w.cols() != (int)g.size())
    throw std::runtime_error("speaker embedding does not match dec.cond");
  for (int c = 0; c < pre.cout; c++) {
    float s = cond_b[c];
    for (int j = 0; j < cond_w.cols(); j++) s += cond_w.at(c, j) * g[j];
    pre.bias[c] += s;
  }
  pre.in = p.input;
  pre.out = addBuffer(p, "pre", pre.cout, 1, addGroup(p, "pre", act_bits));
  p.ops.push_back(std::move(pre));

  int x = p.ops.back().out;
  int rate = 1;
  for (int i = 0; i < 5; i++) {
    std::string sp = "s" + std::to_string(i);
    rate *= kRates[i];
    int res_group = addGroup(p, sp + ".res", act_bits);

    Op up;
    up.kind = OpKind::kConvTranspose;
    up.name = sp + ".ups";
    convTransposeWeights(ws, prefix + ".ups." + std::to_string(i) + ".weight", up);
    if (up.k != kUpKernels[i]) throw std::runtime_error("unexpected kernel size in " + up.name);
    up.stride = kRates[i];
    up.padding = (up.k - up.stride) / 2;
    up.bias = vecOf(ws, prefix + ".ups." + std::to_string(i) + ".bias");
    up.leaky = true;
    up.slope = 0.1f;
    up.in = x;
    up.out = addBuffer(p, up.name, up.cout, rate, res_group);
    int channels = up.cout;
    int up_out = up.out;
    p.ops.push_back(std::move(up));

    int rb_out[3];
    for (int j = 0; j < 3; j++) {
      std::string rp = prefix + ".resblocks." + std::to_string(i * 3 + j);
      int cur = up_out;
      for (int c = 0; c < 3; c++) {
        std::string name = sp + ".rb" + std::to_string(j) + "." + std::to_string(c);
        Op c1;
        c1.kind = OpKind::kConv;
        c1.name = name + ".conv1";
        convWeights(ws, rp + ".convs1." + std::to_string(c) + ".weight", c1);
        if (c1.k != kResKernels[j]) throw std::runtime_error("unexpected kernel size in " + c1.name);
        c1.dilation = kDilations[c];
        c1.padding = c1.dilation * (c1.k - 1) / 2;
        c1.bias = vecOf(ws, rp + ".convs1." + std::to_string(c) + ".bias");
        c1.leaky = true;
        c1.slope = 0.1f;
        c1.in = cur;
        c1.out = addBuffer(p, c1.name, channels, rate, addGroup(p, c1.name, act_bits));
        int mid = c1.out;
        p.ops.push_back(std::move(c1));

        Op c2;
        c2.kind = OpKind::kConv;
        c2.name = name + ".conv2";
        convWeights(ws, rp + ".convs2." + std::to_string(c) + ".weight", c2);
        c2.padding = (c2.k - 1) / 2;
        c2.bias = vecOf(ws, rp + ".convs2." + std::to_string(c) + ".bias");
        c2.leaky = true;
        c2.slope = 0.1f;
        c2.in = mid;
        c2.residual = cur;
        c2.out = addBuffer(p, c2.name, channels, rate, res_group);
        cur = c2.out;
        p.ops.push_back(std::move(c2));
      }
      rb_out[j] = cur;
    }

    Op sum;
    sum.kind = OpKind::kSum3;
    sum.name = sp + ".out";
    sum.cin = sum.cout = channels;
    sum.in = rb_out[0];
    sum.in2 = rb_out[1];
    sum.in3 = rb_out[2];
    sum.out = addBuffer(p, sum.name, channels, rate, addGroup(p, sum.name, act_bits));
    x = sum.out;
    p.ops.push_back(std::move(sum));
  }

  Op post;
  post.kind = OpKind::kOutput;
  post.name = "conv_post";
  convWeights(ws, prefix + ".conv_post.weight", post);
  post.padding = (post.k - 1) / 2;
  post.bias.assign(post.cout, 0.0f);  // conv_post has no bias
  post.leaky = true;
  post.slope = 0.01f;  // F.leaky_relu() default
  post.in = x;
  post.out = addBuffer(p, "pcm", 1, rate, addGroup(p, "pcm", 16, 1.0f / 32767.0f, true));
  p.output = post.out;
  p.ops.push_back(std::move(post));
  return p;
}

}  // namespace tnv
