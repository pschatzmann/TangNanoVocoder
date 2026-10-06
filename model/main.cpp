// Phase 1: fixed-point vocoder model. Calibrates per-layer activation scales
// on sample sentences, runs the integer program next to the float one, and
// reports how far the fixed-point audio is from TinyTTS's float vocoder.
// See docs/fixed-point-model.md.
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
  std::string wav_dir, export_path, dump_dir, export_hw, sentence_out, pcm_out;
  int max_frames = 448;
  int max_tile = 0;
  int crop = 0;
  bool check_hw = false;
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
      "  --export FILE   write the quantized program (docs/fixed-point-model.md)\n"
      "  --dump DIR      write golden vectors of the first evaluated sentence\n"
      "  --export-hw F   write the gateware's SDRAM image (docs/gateware.md)\n"
      "  --max-frames N  longest sentence the image's SDRAM layout holds, in latent frames (default 448)\n"
      "  --max-tile N    cap the output frames per tile (tests the tile loop on short sentences)\n"
      "  --sentence-out F  write the first evaluated sentence as a link 'S' packet\n"
      "  --pcm-out F     write the first evaluated sentence's fixed-point PCM (raw int16)\n"
      "  --crop N        use only the first N latent frames of each evaluated sentence\n"
      "  --check-hw      run the gateware image like the scheduler does and compare its PCM\n"
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
    else if (a == "--crop") o.crop = std::atoi(next());
    else if (a == "--check-hw") o.check_hw = true;
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
