#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Executors.h"
#include "Program.h"

namespace tnv {

namespace detail {

class LeWriter {
 public:
  explicit LeWriter(const std::string& path) : f_(path, std::ios::binary) {
    if (!f_) throw std::runtime_error("cannot write " + path);
  }
  template <typename T>
  void put(T v) {
    for (size_t i = 0; i < sizeof(T); i++) f_.put((char)((uint64_t)v >> (8 * i)));
  }
  void putF32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    put<uint32_t>(u);
  }
  void putStr(const std::string& s) {
    put<uint8_t>((uint8_t)s.size());
    f_.write(s.data(), (std::streamsize)s.size());
  }

 private:
  std::ofstream f_;
};

}  // namespace detail

/// Writes a quantized program in the format documented in
/// docs/studies.md ("Program file"): what the gateware's layer
/// scheduler and its weight streaming read from SDRAM/flash.
inline void exportProgram(const Program& p, const std::string& path) {
  detail::LeWriter w(path);
  w.put<uint32_t>(0x5156'4E54u);  // "TNVQ"
  w.put<uint32_t>(1);
  w.put<uint32_t>((uint32_t)p.groups.size());
  for (const ScaleGroup& g : p.groups) {
    w.put<uint8_t>((uint8_t)g.bits);
    w.put<uint8_t>(g.fixed ? 1 : 0);
    w.putF32(g.scale);
    w.putStr(g.name);
  }
  w.put<uint32_t>((uint32_t)p.buffers.size());
  for (const Buffer& b : p.buffers) {
    w.put<uint16_t>((uint16_t)b.channels);
    w.put<uint16_t>((uint16_t)b.rate);
    w.put<uint16_t>((uint16_t)b.group);
    w.putStr(b.name);
  }
  w.put<uint32_t>((uint32_t)p.input);
  w.put<uint32_t>((uint32_t)p.output);
  w.put<uint32_t>((uint32_t)p.ops.size());
  for (const Op& op : p.ops) {
    w.put<uint8_t>((uint8_t)op.kind);
    w.put<uint8_t>((uint8_t)op.wbits);
    w.put<uint16_t>((uint16_t)op.lr_mul);
    for (int b : {op.in, op.out, op.residual, op.in2, op.in3}) w.put<int16_t>((int16_t)b);
    for (int v : {op.cin, op.cout, op.k, op.dilation, op.stride, op.padding}) w.put<uint16_t>((uint16_t)v);
    w.putStr(op.name);
    if (op.kind != OpKind::kSum3) {
      for (int16_t v : op.wq) {
        if (op.wbits <= 8) w.put<int8_t>((int8_t)v);
        else w.put<int16_t>(v);
      }
      for (int32_t v : op.bq) w.put<int32_t>(v);
    }
    for (int32_t v : op.mult) w.put<uint16_t>((uint16_t)v);
    for (int32_t v : op.shift) w.put<uint8_t>((uint8_t)v);
  }
  const auto& lut = tanhLut();
  w.put<uint32_t>((uint32_t)lut.size());
  for (int32_t v : lut) w.put<int16_t>((int16_t)v);
}

/**
 * @brief Golden vectors for gateware testbenches: the quantized input and
 * every op's output for one sentence, each as raw little-endian int16
 * [frames][channels], plus manifest.txt (one line per file:
 * `file op_index name frames channels bits`).
 */
inline void dumpGolden(const Program& p, const std::vector<int32_t>& zq, int frames, const std::string& dir) {
  std::FILE* manifest = std::fopen((dir + "/manifest.txt").c_str(), "w");
  if (!manifest) throw std::runtime_error("cannot write " + dir + "/manifest.txt (does the directory exist?)");
  auto write = [&](const std::string& file, int op_index, int buffer, const std::vector<int32_t>& v, int nframes) {
    detail::LeWriter w(dir + "/" + file);
    for (int32_t x : v) w.put<int16_t>((int16_t)x);
    const Buffer& b = p.buffers[buffer];
    std::fprintf(manifest, "%s %d %s %d %d %d\n", file.c_str(), op_index, b.name.c_str(), nframes, b.channels,
                 p.groups[b.group].bits);
  };

  IntExecutor ix(p);
  ix.setInputQ(zq, frames);
  write("input_z.bin", -1, p.input, zq, frames);
  for (size_t i = 0; i < p.ops.size(); i++) {
    ix.step(i);
    char file[32];
    std::snprintf(file, sizeof(file), "op%03zu.bin", i);
    int out = p.ops[i].out;
    write(file, (int)i, out, ix.buf(out), ix.frames(out));
  }
  std::fclose(manifest);
}

/// One sentence as the link sends it (vocoder_link.v): 'S', u16 frames,
/// frames x 32 int16 (frame by frame), checksum (sum of payload bytes).
inline void writeSentence(const std::vector<int32_t>& zq, int frames, const std::string& path) {
  detail::LeWriter w(path);
  w.put<uint8_t>(0x53);
  w.put<uint16_t>((uint16_t)frames);
  uint8_t sum = 0;
  for (int32_t v : zq) {
    w.put<int16_t>((int16_t)v);
    sum = (uint8_t)(sum + (uint8_t)(v & 0xFF) + (uint8_t)((v >> 8) & 0xFF));
  }
  w.put<uint8_t>(sum);
}

/// 16-bit mono PCM WAV.
inline void writeWav(const std::string& path, const std::vector<int16_t>& pcm, int sample_rate = 44100) {
  detail::LeWriter w(path);
  uint32_t data_bytes = (uint32_t)pcm.size() * 2;
  w.put<uint32_t>(0x4646'4952u);  // "RIFF"
  w.put<uint32_t>(36 + data_bytes);
  w.put<uint32_t>(0x4556'4157u);  // "WAVE"
  w.put<uint32_t>(0x2074'6D66u);  // "fmt "
  w.put<uint32_t>(16);
  w.put<uint16_t>(1);  // PCM
  w.put<uint16_t>(1);  // mono
  w.put<uint32_t>((uint32_t)sample_rate);
  w.put<uint32_t>((uint32_t)sample_rate * 2);
  w.put<uint16_t>(2);
  w.put<uint16_t>(16);
  w.put<uint32_t>(0x6174'6164u);  // "data"
  w.put<uint32_t>(data_bytes);
  for (int16_t s : pcm) w.put<int16_t>(s);
}

}  // namespace tnv
