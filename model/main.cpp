// Phase 1: fixed-point vocoder model. Calibrates per-layer activation scales
// on sample sentences, runs the integer program next to the float one, and
// reports how far the fixed-point audio is from TinyTTS's float vocoder.
// See docs/studies.md.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Executors.h"
#include "Export.h"
#include "HwImage.h"
#include "HwSim.h"
#include "FlowFixed.h"
#include "Spectral.h"
#include "TangNanoVocoder/Latent.h"
#include "Program.h"
#include "Quantize.h"
#include "TinyTTS/TinyTTSCore.h"

using namespace tnv;

namespace {

const char* kCalibrationSentences[] = {
    "The quick brown fox jumps over the lazy dog.",
    "Hello world!",
    "Please turn left at the next intersection.",
    "It is a beautiful day, isn't it?",
    "Seven hundred and forty two people attended the meeting.",
    "Can you hear me now?",
    "Warning: battery level is low.",
    "She sells sea shells by the sea shore.",
    "The temperature today will reach twenty five degrees.",
    "Thank you for your patience.",
    "What time does the train to Zurich leave?",
    "Good morning, how are you?",
};

const char* kEvalSentences[] = {
    "This is a test of the fixed point vocoder.",
    "Open the pod bay doors, please.",
    "My phone number is five five five, one two three four.",
    "Rain is expected later this evening.",
    "Do you want to play a game?",
    "The meeting has been moved to Thursday afternoon.",
    "Welcome home!",
    "An apple a day keeps the doctor away.",
};

struct Options {
  std::string tinytts_dir = TNV_TINYTTS_DIR;
  int act_bits = 16;
  int z_bits = 16;
  int convt_wbits = 12;
  double pct = 100.0;
  double headroom = 1.0;
  std::vector<std::string> texts;
  std::string wav_dir, export_path, dump_dir, export_hw, sentence_out, pcm_out, z_out;
  int max_frames = 448;
  int max_tile = 0;
  int crop = 0;
  bool check_hw = false;
  bool flow = false;
  int trace_op = 0;
  std::string trace_out;
  std::string flow_dump;
  FlowConfig flow_cfg;
  bool all_ops = false;
};

void usage() {
  std::printf(
      "usage: vocoder_model [options]\n"
      "  --bits N        activation width in bits (default 16)\n"
      "  --z-bits N      latent width on the ESP32 link (default 16)\n"
      "  --convt-wbits N weight width of the transposed convolutions (default 12)\n"
      "  --calib P       range = P-th percentile of |x| instead of max (e.g. 99.99)\n"
      "  --headroom F    multiply every calibrated range by F (default 1)\n"
      "  --text TEXT     evaluate this sentence instead of the built-in set (repeatable)\n"
      "  --wav DIR       write float / fixed / TinyTTS-int8 WAVs of each evaluated sentence\n"
      "  --export FILE   write the quantized program (docs/studies.md)\n"
      "  --dump DIR      write golden vectors of the first evaluated sentence\n"
      "  --export-hw F   write the gateware's SDRAM image (docs/gateware.md)\n"
      "  --max-frames N  longest sentence the image's SDRAM layout holds, in latent frames (default 448)\n"
      "  --max-tile N    cap the output frames per tile (tests the tile loop on short sentences)\n"
      "  --sentence-out F  write the first evaluated sentence as a link 'S' packet\n"
      "  --pcm-out F     write the first evaluated sentence's fixed-point PCM (raw int16)\n"
      "  --crop N        use only the first N latent frames of each evaluated sentence\n"
      "  --check-hw      run the gateware image like the scheduler does and compare its PCM\n"
      "  --flow          the flow in fixed point too, end to end (docs/studies.md, chapter 3)\n"
      "  --flow-wbits N  flow weight width (default 8)   --flow-bits N  flow activation width (default 16)\n"
      "  --flow-exp-bits N, --flow-rsqrt-bits N  softmax / LayerNorm table sizes (default 8)\n"
      "  --flow-headroom F  multiply the flow's calibrated ranges by F (default 1)\n"
      "  --flow-dump DIR  golden vectors of the first LayerNorm and attention (first sentence)\n"
      "  --z-out F       with --flow: the first sentence's z after the fixed-point flow (int16 [T][32])\n"
      "  --trace-op K F  with --flow: run the image's first K ops (HwSim) on the first sentence and write\n"
      "                  op K-1's output buffer as a tb_gate expect file\n"
      "                  (--sentence-out then writes z_p, --pcm-out the PCM of flow + vocoder)\n"
      "  --all-ops       per-op table for every op, not just stage boundaries\n"
      "  --tinytts DIR   TinyTTS checkout (default %s)\n",
      TNV_TINYTTS_DIR);
}

bool parseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "--bits") o.act_bits = std::atoi(next());
    else if (a == "--z-bits") o.z_bits = std::atoi(next());
    else if (a == "--convt-wbits") o.convt_wbits = std::atoi(next());
    else if (a == "--calib") o.pct = std::atof(next());
    else if (a == "--headroom") o.headroom = std::atof(next());
    else if (a == "--text") o.texts.push_back(next());
    else if (a == "--wav") o.wav_dir = next();
    else if (a == "--export") o.export_path = next();
    else if (a == "--dump") o.dump_dir = next();
    else if (a == "--all-ops") o.all_ops = true;
    else if (a == "--export-hw") o.export_hw = next();
    else if (a == "--max-frames") o.max_frames = std::atoi(next());
    else if (a == "--max-tile") o.max_tile = std::atoi(next());
    else if (a == "--sentence-out") o.sentence_out = next();
    else if (a == "--pcm-out") o.pcm_out = next();
    else if (a == "--z-out") o.z_out = next();
    else if (a == "--crop") o.crop = std::atoi(next());
    else if (a == "--check-hw") o.check_hw = true;
    else if (a == "--flow") o.flow = true;
    else if (a == "--trace-op") {
      o.trace_op = std::atoi(next());
      o.trace_out = next();
    }
    else if (a == "--flow-dump") o.flow_dump = next();
    else if (a == "--flow-wbits") o.flow_cfg.weight_bits = std::atoi(next());
    else if (a == "--flow-bits") o.flow_cfg.act_bits = std::atoi(next());
    else if (a == "--flow-exp-bits") o.flow_cfg.exp_lut_bits = std::atoi(next());
    else if (a == "--flow-rsqrt-bits") o.flow_cfg.rsqrt_lut_bits = std::atoi(next());
    else if (a == "--flow-headroom") o.flow_cfg.headroom = (float)std::atof(next());
    else if (a == "--tinytts") o.tinytts_dir = next();
    else {
      usage();
      return false;
    }
  }
  if (o.act_bits < 4 || o.act_bits > 16 || o.z_bits < 4 || o.z_bits > 16 || o.convt_wbits < 4 ||
      o.convt_wbits > 16) {
    std::fprintf(stderr, "bit widths must be 4..16\n");
    return false;
  }
  return true;
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    std::exit(1);
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

double snrDb(double signal, double noise) { return noise > 0 ? 10.0 * std::log10(signal / noise) : 999.0; }

struct Energy {
  double signal = 0, noise = 0;
  void add(double ref, double test) {
    signal += ref * ref;
    noise += (ref - test) * (ref - test);
  }
  double snr() const { return snrDb(signal, noise); }
};

template <typename V>
Energy compare(const V& ref, const std::vector<float>& test) {
  Energy e;
  for (size_t n = 0; n < ref.size() && n < test.size(); n++) e.add(ref[n], test[n]);
  return e;
}

std::vector<int16_t> toPcm(const std::vector<float>& audio) {
  std::vector<int16_t> pcm(audio.size());
  for (size_t n = 0; n < audio.size(); n++) pcm[n] = (int16_t)std::lround(std::clamp(audio[n], -1.0f, 1.0f) * 32767.0f);
  return pcm;
}

int bitsFor(int64_t max_abs) {
  int b = 1;  // sign
  while (max_abs > 0) {
    b++;
    max_abs >>= 1;
  }
  return b;
}

const char* kindName(OpKind k) {
  switch (k) {
    case OpKind::kConv: return "conv";
    case OpKind::kConvTranspose: return "convT";
    case OpKind::kSum3: return "sum3";
    case OpKind::kOutput: return "output";
  }
  return "?";
}

/// --flow: the flow in fixed point as well, end to end through the fixed-point vocoder.
int flowStudy(const tinytts::TinyTTSCore& core, Program& prog, const Options& opt,
              const std::vector<std::string>& calib_texts, const std::vector<std::string>& eval_texts) {
  const FlowConfig& cfg = opt.flow_cfg;
  auto prior = [&](const std::string& text, uint32_t seed) { return latentPrior(core, text, 0, 0.667f, 1.0f, seed); };
  auto gmat = [](const std::vector<float>& g) {
    tinytts::Mat m(1, (int)g.size());
    for (size_t c = 0; c < g.size(); c++) m.at(0, (int)c) = g[c];
    return m;
  };
  auto snr = [](const auto& ref, const auto& test) {
    double s = 0, n = 0;
    for (size_t i = 0; i < ref.size() && i < test.size(); i++) {
      s += (double)ref[i] * ref[i];
      n += ((double)ref[i] - test[i]) * ((double)ref[i] - test[i]);
    }
    return std::make_pair(s, n);
  };

  FlowFixed flow(cfg);
  Latent first = prior(calib_texts[0], 100);
  flow.build(core.weights(), first.g);
  for (size_t i = 0; i < calib_texts.size(); i++) flow.runFloat(prior(calib_texts[i], 100 + (uint32_t)i).z, true);
  // one scale for the latent: the flow's stream and the vocoder's input, so
  // the flow's integers go straight into the vocoder (as on the FPGA)
  ScaleGroup& vz = prog.groups[prog.buffers[prog.input].group];
  float z_range = std::max(flow.zRange(), vz.scale * vz.qmax());
  flow.quantize(z_range);
  vz.scale = z_range / vz.qmax();
  quantize(prog);
  std::printf("fixed-point flow: %d-bit activations, %d-bit weights (%d parameters), exp table %d, rsqrt table %d, "
              "headroom %.2f\n\n", cfg.act_bits, cfg.weight_bits, flow.paramCount(), 1 << cfg.exp_lut_bits,
              1 << cfg.rsqrt_lut_bits, cfg.headroom);

  if (!opt.flow_dump.empty()) {
    Latent lp = prior(eval_texts[0], 200);
    if (opt.crop > 0 && lp.z.rows() > opt.crop) {
      tinytts::Mat z(opt.crop, lp.z.cols());
      for (int t = 0; t < opt.crop; t++)
        for (int c = 0; c < lp.z.cols(); c++) z.at(t, c) = lp.z.at(t, c);
      lp.z = std::move(z);
    }
    flow.capture = FlowFixed::Capture();
    flow.runFixed(lp.z);
    const FlowFixed::Capture& cap = flow.capture;
    std::string d = opt.flow_dump;
    auto hexfile = [&](const std::string& name, const std::vector<int64_t>& v, int digits) {
      std::FILE* f = std::fopen((d + "/" + name).c_str(), "w");
      if (!f) throw std::runtime_error("cannot write " + d + "/" + name);
      for (int64_t x : v) std::fprintf(f, "%0*llx\n", digits, (unsigned long long)(x & ((1ull << (4 * digits)) - 1)));
      std::fclose(f);
    };
    auto v64 = [](const std::vector<int32_t>& v) { return std::vector<int64_t>(v.begin(), v.end()); };
    int T = lp.z.rows();
    hexfile("ln_in.hex", v64(cap.ln_in), 4);
    hexfile("ln_out.hex", v64(cap.ln_out), 4);
    std::vector<int64_t> lnp = {T, cap.norm.eps, cap.norm.gs};
    for (int c = 0; c < 32; c++) lnp.push_back(cap.norm.g[c]);
    for (int c = 0; c < 32; c++) lnp.push_back(cap.norm.b[c]);
    hexfile("ln_params.hex", lnp, 8);
    hexfile("att_q.hex", v64(cap.q), 4);
    hexfile("att_k.hex", v64(cap.k), 4);
    hexfile("att_v.hex", v64(cap.v), 4);
    hexfile("att_out.hex", v64(cap.merged), 4);
    const AttnParams& a = cap.attn;
    std::vector<int64_t> ap = {T, a.score_mult, a.score_shift, a.merge_mult, a.merge_shift};
    for (int32_t x : a.rel_k) ap.push_back(x);
    for (int32_t x : a.rel_v) ap.push_back(x);
    hexfile("att_params.hex", ap, 8);
    // the gateware's table ROMs: entry i = lut[i + 1] << 17 | lut[i] (one read gives both)
    auto pairs = [](const std::vector<int64_t>& lut) {
      std::vector<int64_t> p;
      for (size_t i = 0; i + 1 < lut.size(); i++) p.push_back(lut[i + 1] << 17 | lut[i]);
      return p;
    };
    hexfile("exp2.hex", pairs(flow.tables().exp_lut), 9);
    hexfile("rsqrt.hex", pairs(flow.tables().rsqrt_lut), 9);
    std::printf("wrote flow unit golden vectors to %s (%d frames)\n", d.c_str(), T);
  }

  if (!opt.sentence_out.empty() || !opt.pcm_out.empty() || !opt.z_out.empty()) {
    // the first sentence (cropped) as the link carries it with the flow: z_p
    Latent lp = prior(eval_texts[0], 200);
    if (opt.crop > 0 && lp.z.rows() > opt.crop) {
      tinytts::Mat z(opt.crop, lp.z.cols());
      for (int t = 0; t < opt.crop; t++)
        for (int c = 0; c < lp.z.cols(); c++) z.at(t, c) = lp.z.at(t, c);
      lp.z = std::move(z);
    }
    std::vector<int32_t> zp_q(lp.z.data().size()), zq;
    for (size_t n = 0; n < zp_q.size(); n++)
      zp_q[n] = std::clamp<long>(std::lround(lp.z.data()[n] / flow.zScale()), -32767, 32767);
    flow.runFixed(lp.z, &zq);
    if (!opt.sentence_out.empty()) writeSentence(zp_q, lp.z.rows(), opt.sentence_out);
    if (!opt.z_out.empty()) {
      detail::LeWriter w(opt.z_out);
      for (int32_t v : zq) w.put<int16_t>((int16_t)v);
    }
    if (!opt.pcm_out.empty()) {
      IntExecutor ix(prog);
      ix.setInputQ(zq, lp.z.rows());
      ix.run();
      detail::LeWriter w(opt.pcm_out);
      for (int32_t v : ix.buf(prog.output)) w.put<int16_t>((int16_t)v);
    }
    std::printf("first sentence with the flow: %d frames (z_p packet, z, PCM written as requested)\n", lp.z.rows());
  }

  if (opt.check_hw || !opt.export_hw.empty()) {
    // the hardware image with the flow; the longest sentence shrinks until the layout fits
    HwImage img;
    for (int mf = opt.max_frames;; mf -= 64) {
      try {
        img = buildHwImage(prog, mf, opt.max_tile, &flow);
        break;
      } catch (const std::runtime_error& e) {
        if (mf <= 64 || std::string(e.what()).find("SDRAM layout") == std::string::npos) throw;
      }
    }
    std::printf("hardware image with the flow: %zu words (%d flow ops), sentences up to %d frames, SDRAM %u of %u words\n",
                img.words.size(), img.flow_ops, img.max_frames, img.used_words, HwImage::kSdramWords);
    if (!opt.export_hw.empty()) {
      detail::LeWriter w(opt.export_hw);
      for (uint32_t v : img.words) w.put<uint32_t>(v);
      std::printf("wrote %s\n", opt.export_hw.c_str());
    }
    if (opt.trace_op > 0) {
      Latent lp = prior(eval_texts[0], 200);
      int T = opt.crop > 0 ? std::min(opt.crop, lp.z.rows()) : lp.z.rows();
      std::vector<int32_t> zp_q((size_t)T * 32);
      for (size_t n = 0; n < zp_q.size(); n++)
        zp_q[n] = std::clamp<long>(std::lround(lp.z.data()[n] / flow.zScale()), -32767, 32767);
      std::vector<uint32_t> words = img.words;
      words[2] = (uint32_t)opt.trace_op;
      HwSim sim(words);
      sim.run(zp_q, T);
      const uint32_t* d = &words[words[3] + (opt.trace_op - 1) * HwImage::kDescWords];
      int op_class = d[0] >> 6 & 3;
      int C = op_class == 0 ? (int)(d[1] & 0xFF) : 32;
      int rate = (int)(d[10] >> 16);
      std::vector<int16_t> v = sim.readPlanes(d[4], d[5], C, T * rate);
      std::FILE* f = std::fopen(opt.trace_out.c_str(), "w");
      std::fprintf(f, "%u %u %d %d\n", d[4], d[5], C, T * rate);
      for (int16_t x : v) std::fprintf(f, "%04x\n", (uint16_t)x);
      std::fclose(f);
      std::printf("op %d (class %d): output buffer, %d channels x %d frames -> %s\n", opt.trace_op - 1, op_class, C,
                  T * rate, opt.trace_out.c_str());
    }
    if (opt.check_hw) {
      int bad_total = 0;
      for (size_t i = 0; i < eval_texts.size(); i++) {
        Latent lp = prior(eval_texts[i], 200 + (uint32_t)i);
        if (lp.z.rows() > img.max_frames) {
          std::printf("check-hw %-40.40s skipped: %d frames\n", eval_texts[i].c_str(), lp.z.rows());
          continue;
        }
        std::vector<int32_t> zq, zp_q(lp.z.data().size());
        for (size_t n = 0; n < zp_q.size(); n++)
          zp_q[n] = std::clamp<long>(std::lround(lp.z.data()[n] / flow.zScale()), -32767, 32767);
        flow.runFixed(lp.z, &zq);
        IntExecutor ix(prog);
        ix.setInputQ(zq, lp.z.rows());
        ix.run();
        const auto& ref = ix.buf(prog.output);
        HwSim sim(img.words);
        std::vector<int16_t> pcm = sim.run(zp_q, lp.z.rows());
        int bad = 0;
        for (size_t n = 0; n < ref.size(); n++) bad += pcm[n] != ref[n];
        double compute_s = sim.cycles().total / 54e6, audio_s = ref.size() / 44100.0;
        std::printf("check-hw %-40.40s %6zu samples, %d differ; estimated %.2fs at 54MHz = %.2fx real time\n",
                    eval_texts[i].c_str(), ref.size(), bad, compute_s, compute_s / audio_s);
        bad_total += bad;
      }
      std::printf("check-hw: %s\n\n", bad_total == 0 ? "image (flow + vocoder) matches the model" : "MISMATCH");
      if (bad_total) return 2;
    }
  }

  std::printf("%-44s %6s %8s %9s %9s %9s\n", "sentence", "frames", "graph dB", "z dB", "today dB", "flow dB");
  double zs = 0, zn = 0, a_s = 0, a_n = 0, b_s = 0, b_n = 0;
  SpectralCompare spec_today, spec_flow, spec_int8;
  for (size_t i = 0; i < eval_texts.size(); i++) {
    Latent lp = prior(eval_texts[i], 200 + (uint32_t)i);
    tinytts::Mat g = gmat(lp.g);
    tinytts::Mat z_ref = core.flow().reverse(lp.z, g);  // TinyTTS, float
    tinytts::Mat z_flt = flow.runFloat(lp.z);          // the same graph here, float
    std::vector<int32_t> zq;
    tinytts::Mat z_fix = flow.runFixed(lp.z, &zq);     // fixed point

    tinytts::ops::setDecoderPrecision(tinytts::ops::DecoderPrecision::kFloat32);
    auto ref_ps = core.vocoder().forward(z_ref, g);  // all float
    std::vector<float> ref(ref_ps.begin(), ref_ps.end());
    auto pcm_of = [&](const tinytts::Mat& z) {
      IntExecutor ix(prog);
      ix.setInput(z);
      ix.run();
      const auto& q = ix.buf(prog.output);
      std::vector<float> out(q.size());
      for (size_t n = 0; n < q.size(); n++) out[n] = q[n] / 32767.0f;
      return out;
    };
    std::vector<float> today = pcm_of(z_ref), both;
    {  // the flow's integers straight into the vocoder
      IntExecutor ix(prog);
      ix.setInputQ(zq, lp.z.rows());
      ix.run();
      const auto& q = ix.buf(prog.output);
      both.resize(q.size());
      for (size_t n = 0; n < q.size(); n++) both[n] = q[n] / 32767.0f;
    }
    tinytts::ops::setDecoderPrecision(tinytts::ops::DecoderPrecision::kInt8Activations);
    auto dyn_ps = core.vocoder().forward(z_ref, g);  // TinyTTS's own INT8 vocoder: a known-acceptable reference
    std::vector<float> dyn(dyn_ps.begin(), dyn_ps.end());
    tinytts::ops::setDecoderPrecision(tinytts::ops::DecoderPrecision::kFloat32);
    spec_today.add(ref, today);
    spec_flow.add(ref, both);
    spec_int8.add(ref, dyn);

    auto gch = snr(z_ref.data(), z_flt.data());
    auto zc = snr(z_ref.data(), z_fix.data());
    auto ac = snr(ref, today);
    auto bc = snr(ref, both);
    zs += zc.first; zn += zc.second; a_s += ac.first; a_n += ac.second; b_s += bc.first; b_n += bc.second;
    auto db = [](std::pair<double, double> p) { return p.second > 0 ? 10 * std::log10(p.first / p.second) : 999.0; };
    std::printf("%-44.44s %6d %8.1f %9.2f %9.2f %9.2f\n", eval_texts[i].c_str(), lp.z.rows(), db(gch), db(zc),
                db(ac), db(bc));
    if (!opt.wav_dir.empty()) {
      auto pcm16 = [](const std::vector<float>& a) {
        std::vector<int16_t> o(a.size());
        for (size_t n = 0; n < a.size(); n++) o[n] = (int16_t)std::lround(std::clamp(a[n], -1.0f, 1.0f) * 32767);
        return o;
      };
      std::string base = opt.wav_dir + "/f" + std::to_string(i);
      writeWav(base + "_float.wav", pcm16(ref));
      writeWav(base + "_today.wav", pcm16(today));
      writeWav(base + "_fixed_flow.wav", pcm16(both));
    }
  }
  auto db2 = [](double s, double n) { return n > 0 ? 10 * std::log10(s / n) : 999.0; };
  std::printf("\nall sentences, against TinyTTS in float:\n");
  std::printf("  z from the fixed-point flow:                 %6.2f dB\n", db2(zs, zn));
  std::printf("  PCM, float flow + fixed vocoder (today):     %6.2f dB\n", db2(a_s, a_n));
  std::printf("  PCM, fixed flow + fixed vocoder (proposal):  %6.2f dB\n", db2(b_s, b_n));
  std::printf("\nphase-insensitive (STFT magnitudes), against TinyTTS in float:   spectral SNR   log-spectral distance\n");
  std::printf("  TinyTTS's own INT8 vocoder (reference):     %6.2f dB   %5.2f dB\n", spec_int8.snrDb(), spec_int8.lsdDb());
  std::printf("  float flow + fixed vocoder (today):         %6.2f dB   %5.2f dB\n", spec_today.snrDb(), spec_today.lsdDb());
  std::printf("  fixed flow + fixed vocoder (proposal):      %6.2f dB   %5.2f dB\n", spec_flow.snrDb(), spec_flow.lsdDb());
  std::printf("  widest flow accumulator: %d bits; requantize shifts %d..%d\n", flow.accBits(),
              flow.convShiftRange().first, flow.convShiftRange().second);
  std::printf("  flow values saturated: %llu\n%s", (unsigned long long)flow.saturated(), flow.saturationReport().c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(argc, argv, opt)) return 1;

  // ---- TinyTTS: weights, dictionary, text -> latent
  std::string research = opt.tinytts_dir + "/research/";
  static std::vector<uint8_t> weights = readFile(research + "weights.bin");
  static std::vector<uint8_t> cmudict = readFile(research + "cmudict.bin");
  static std::vector<uint8_t> dict_model = readFile(research + "dictionary_model.bin");
  static tinytts::TinyTTSCore core;
  if (!core.begin(weights.data(), weights.size(), cmudict.data(), cmudict.size(), 2, 4, 4, dict_model.data(),
                  dict_model.size())) {
    std::fprintf(stderr, "TinyTTS failed to load %s\n", research.c_str());
    return 1;
  }

  auto latents = [&](const std::vector<std::string>& texts, uint32_t seed0) {
    std::vector<Latent> out;
    for (size_t i = 0; i < texts.size(); i++) out.push_back(latentFromText(core, texts[i], 0, 0.667f, 1.0f, seed0 + (uint32_t)i));
    return out;
  };
  std::vector<std::string> calib_texts(std::begin(kCalibrationSentences), std::end(kCalibrationSentences));
  std::vector<std::string> eval_texts = opt.texts;
  if (eval_texts.empty()) eval_texts.assign(std::begin(kEvalSentences), std::end(kEvalSentences));
  std::vector<Latent> calib = latents(calib_texts, 100);
  std::vector<Latent> eval = latents(eval_texts, 200);
  if (opt.crop > 0)
    for (Latent& l : eval) {
      if (l.z.rows() <= opt.crop) continue;
      tinytts::Mat z(opt.crop, l.z.cols());
      for (int t = 0; t < opt.crop; t++)
        for (int c = 0; c < l.z.cols(); c++) z.at(t, c) = l.z.at(t, c);
      l.z = std::move(z);
    }

  // ---- program, calibration, quantization
  Program prog = buildProgram(core.weights(), calib[0].g, opt.act_bits, opt.z_bits);
  Calibrator cal(prog);
  int calib_frames = 0;
  for (const Latent& l : calib) {
    cal.addSentence(l.z);
    calib_frames += l.z.rows();
  }
  applyCalibration(prog, cal, opt.pct, opt.headroom);
  quantize(prog, opt.convt_wbits);

  std::printf("fixed-point vocoder: %d-bit activations, %d-bit latent, %d-bit upsampling weights, ranges from %s x %.2f\n",
              opt.act_bits, opt.z_bits, opt.convt_wbits,
              opt.pct >= 100.0 ? "max |x|" : (std::to_string(opt.pct) + "th percentile").c_str(), opt.headroom);
  std::printf("calibration: %zu sentences, %.1fs of audio; evaluation: %zu sentences\n\n", calib.size(),
              calib_frames * 512 / 44100.0, eval.size());

  if (opt.flow) return flowStudy(core, prog, opt, calib_texts, eval_texts);

  if (opt.check_hw) {
    HwImage img = buildHwImage(prog, opt.max_frames, opt.max_tile);
    int bad_total = 0;
    for (size_t s = 0; s < eval.size(); s++) {
      IntExecutor ix(prog);
      ix.setInput(eval[s].z);
      std::vector<int32_t> zq = ix.buf(prog.input);
      ix.run();
      HwSim sim(img.words);
      std::vector<int16_t> pcm = sim.run(zq, eval[s].z.rows());
      const auto& ref = ix.buf(prog.output);
      int bad = 0;
      for (size_t n = 0; n < ref.size(); n++) bad += pcm[n] != ref[n];
      const HwSim::Cycles& c = sim.cycles();
      double audio_s = ref.size() / 44100.0, compute_s = c.total / 54e6;
      std::printf("check-hw %-40.40s %6zu samples, %d differ; estimated %.2fs at 54MHz = %.2fx real time "
                  "(engine %.0f%%, SDRAM %.0f%%)\n",
                  eval_texts[s].c_str(), ref.size(), bad, compute_s, compute_s / audio_s, 100 * c.engine / c.total,
                  100 * c.dma / c.total);
      bad_total += bad;
    }
    std::printf("check-hw: %s\n\n", bad_total == 0 ? "image matches the model" : "MISMATCH");
    if (bad_total) return 2;
  }

  // ---- evaluation: float program, integer program and TinyTTS in lockstep
  std::vector<Energy> per_op(prog.ops.size());
  Energy input_err, graph_check, fixed_total, tinytts_int8_total;
  std::vector<OpStats> stats(prog.ops.size());
  std::printf("%-56s %6s %9s %9s\n", "sentence", "sec", "fixed dB", "int8 dB");
  for (size_t s = 0; s < eval.size(); s++) {
    const Latent& l = eval[s];
    tinytts::Mat g(1, (int)l.g.size());
    for (size_t c = 0; c < l.g.size(); c++) g.at(0, (int)c) = l.g[c];

    tinytts::ops::setDecoderPrecision(tinytts::ops::DecoderPrecision::kFloat32);
    auto ref_ps = core.vocoder().forward(l.z, g);
    std::vector<float> ref(ref_ps.begin(), ref_ps.end());
    tinytts::ops::setDecoderPrecision(tinytts::ops::DecoderPrecision::kInt8Activations);
    auto dyn_ps = core.vocoder().forward(l.z, g);
    std::vector<float> dyn(dyn_ps.begin(), dyn_ps.end());
    tinytts::ops::setDecoderPrecision(tinytts::ops::DecoderPrecision::kFloat32);

    FloatExecutor fx(prog);
    IntExecutor ix(prog);
    fx.setInput(l.z);
    ix.setInput(l.z);
    float zs = prog.groups[prog.buffers[prog.input].group].scale;
    for (size_t n = 0; n < fx.buf(prog.input).size(); n++) input_err.add(fx.buf(prog.input)[n], ix.buf(prog.input)[n] * zs);
    for (size_t i = 0; i < prog.ops.size(); i++) {
      fx.step(i);
      ix.step(i);
      int out = prog.ops[i].out;
      float sc = prog.groups[prog.buffers[out].group].scale;
      const auto &fv = fx.buf(out);
      const auto &iv = ix.buf(out);
      for (size_t n = 0; n < fv.size(); n++) per_op[i].add(fv[n], iv[n] * sc);
    }
    for (size_t i = 0; i < stats.size(); i++) {
      stats[i].max_acc = std::max(stats[i].max_acc, ix.stats()[i].max_acc);
      stats[i].saturated += ix.stats()[i].saturated;
      stats[i].outputs += ix.stats()[i].outputs;
    }

    const auto& pcm_q = ix.buf(prog.output);
    std::vector<float> fixed(pcm_q.size());
    for (size_t n = 0; n < fixed.size(); n++) fixed[n] = pcm_q[n] / 32767.0f;
    Energy gc = compare(ref, fx.buf(prog.output));
    Energy fe = compare(ref, fixed);
    Energy de = compare(ref, dyn);
    graph_check.signal += gc.signal;
    graph_check.noise += gc.noise;
    fixed_total.signal += fe.signal;
    fixed_total.noise += fe.noise;
    tinytts_int8_total.signal += de.signal;
    tinytts_int8_total.noise += de.noise;
    std::printf("%-56.56s %6.2f %9.2f %9.2f\n", eval_texts[s].c_str(), ref.size() / 44100.0, fe.snr(), de.snr());

    if (!opt.wav_dir.empty()) {
      std::string base = opt.wav_dir + "/e" + std::to_string(s);
      std::vector<int16_t> pcm16(pcm_q.begin(), pcm_q.end());
      writeWav(base + "_float.wav", toPcm(ref));
      writeWav(base + "_fixed.wav", pcm16);
      writeWav(base + "_tinytts_int8.wav", toPcm(dyn));
    }
    if (s == 0 && !opt.sentence_out.empty()) {
      IntExecutor in(prog);
      in.setInput(l.z);
      writeSentence(in.buf(prog.input), l.z.rows(), opt.sentence_out);
    }
    if (s == 0 && !opt.pcm_out.empty()) {
      detail::LeWriter w(opt.pcm_out);
      for (int32_t v : pcm_q) w.put<int16_t>((int16_t)v);
    }
    if (s == 0 && !opt.dump_dir.empty()) {
      IntExecutor in(prog);
      in.setInput(l.z);
      dumpGolden(prog, in.buf(prog.input), l.z.rows(), opt.dump_dir);
    }
  }

  std::printf("\nSNR against TinyTTS's float vocoder, all sentences:\n");
  std::printf("  fixed-point model (this):            %6.2f dB\n", fixed_total.snr());
  std::printf("  TinyTTS dynamic per-timestep INT8:   %6.2f dB\n", tinytts_int8_total.snr());
  std::printf("  float program (graph check):         %6.2f dB\n", graph_check.snr());
  std::printf("  latent on the link (%d-bit):          %6.2f dB\n", opt.z_bits, input_err.snr());

  std::printf("\n%-4s %-22s %-6s %10s %8s %9s %8s\n", "op", "name", "kind", "range", "SNR dB", "sat ppm", "acc bits");
  int acc_bits_max = 0;
  uint64_t sat_total = 0, out_total = 0;
  for (size_t i = 0; i < prog.ops.size(); i++) {
    const Op& op = prog.ops[i];
    const ScaleGroup& g = prog.groups[prog.buffers[op.out].group];
    int acc_bits = bitsFor(stats[i].max_acc);
    acc_bits_max = std::max(acc_bits_max, acc_bits);
    sat_total += stats[i].saturated;
    out_total += stats[i].outputs;
    bool boundary = op.kind != OpKind::kConv || op.name == "conv_pre";
    if (!opt.all_ops && !boundary) continue;
    std::printf("%-4zu %-22s %-6s %10.4f %8.2f %9.1f %8d\n", i, op.name.c_str(), kindName(op.kind),
                g.scale * g.qmax(), per_op[i].snr(), 1e6 * stats[i].saturated / std::max<uint64_t>(1, stats[i].outputs),
                acc_bits);
  }
  std::printf("\nwidest accumulator: %d bits; saturated outputs: %.1f ppm overall\n", acc_bits_max,
              1e6 * sat_total / std::max<uint64_t>(1, out_total));

  if (!opt.export_path.empty()) {
    exportProgram(prog, opt.export_path);
    std::printf("wrote program to %s\n", opt.export_path.c_str());
  }
  if (!opt.dump_dir.empty()) std::printf("wrote golden vectors to %s\n", opt.dump_dir.c_str());
  if (!opt.export_hw.empty()) {
    HwImage img = buildHwImage(prog, opt.max_frames, opt.max_tile);
    detail::LeWriter w(opt.export_hw);
    for (uint32_t v : img.words) w.put<uint32_t>(v);
    std::printf("wrote gateware image to %s: %zu words, SDRAM layout %u of %u words, sentences up to %d frames\n",
                opt.export_hw.c_str(), img.words.size(), img.used_words, HwImage::kSdramWords, img.max_frames);
  }
  if (!opt.sentence_out.empty()) std::printf("wrote sentence packet to %s\n", opt.sentence_out.c_str());
  return 0;
}
