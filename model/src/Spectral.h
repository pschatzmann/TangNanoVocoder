#pragma once
#include <cmath>
#include <complex>
#include <vector>

namespace tnv {

/// Phase-insensitive comparison of two signals: STFT magnitudes (Hann,
/// 1024 samples, hop 256). Waveform SNR punishes phase differences a GAN
/// vocoder produces from tiny input changes, which are inaudible.
struct SpectralCompare {
  double sig = 0, err = 0;      ///< magnitude energy, magnitude error energy
  double lsd_sum = 0;           ///< sum over frames of the RMS log-spectral distance (dB)
  int frames = 0;

  /// Both signals are rounded to 16-bit PCM first (as played): otherwise a
  /// float reference's near-silent bins dominate the log-spectral distance.
  void add(const std::vector<float>& ref_f, const std::vector<float>& test_f) {
    auto pcm = [](const std::vector<float>& v) {
      std::vector<float> o(v.size());
      for (size_t i = 0; i < v.size(); i++) o[i] = std::round(std::fmax(-1.0f, std::fmin(1.0f, v[i])) * 32767.0f) / 32767.0f;
      return o;
    };
    std::vector<float> ref = pcm(ref_f), test = pcm(test_f);
    const int n = 1024, hop = 256, bins = n / 2 + 1;
    std::vector<double> win(n);
    for (int i = 0; i < n; i++) win[i] = 0.5 - 0.5 * std::cos(2 * M_PI * i / n);
    std::vector<std::complex<double>> a(n), b(n);
    size_t len = std::min(ref.size(), test.size());
    for (size_t start = 0; start + n <= len; start += hop) {
      for (int i = 0; i < n; i++) {
        a[i] = ref[start + i] * win[i];
        b[i] = test[start + i] * win[i];
      }
      fft(a);
      fft(b);
      double e = 0, d = 0, lsd = 0;
      for (int k = 0; k < bins; k++) {
        double ma = std::abs(a[k]), mb = std::abs(b[k]);
        e += ma * ma;
        d += (ma - mb) * (ma - mb);
        double la = 20 * std::log10(ma + 1e-3), lb = 20 * std::log10(mb + 1e-3);
        lsd += (la - lb) * (la - lb);
      }
      if (e < 1e-3) continue;  // silence
      sig += e;
      err += d;
      lsd_sum += std::sqrt(lsd / bins);
      frames++;
    }
  }
  double snrDb() const { return err > 0 ? 10 * std::log10(sig / err) : 999; }
  double lsdDb() const { return frames ? lsd_sum / frames : 0; }

 private:
  static void fft(std::vector<std::complex<double>>& x) {
    size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; i++) {
      size_t bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
      std::complex<double> w = std::polar(1.0, -2 * M_PI / len);
      for (size_t i = 0; i < n; i += len) {
        std::complex<double> wk = 1;
        for (size_t k = 0; k < len / 2; k++) {
          std::complex<double> u = x[i + k], v = x[i + k + len / 2] * wk;
          x[i + k] = u + v;
          x[i + k + len / 2] = u - v;
          wk *= w;
        }
      }
    }
  }
};

}  // namespace tnv
