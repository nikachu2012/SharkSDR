#pragma once

#include <SharkSDR/export.h>
#include <SharkSDR/types.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <type_traits>
#include <vector>

namespace SharkSDR::dsp {

// ---- FIR design -------------------------------------------------------------

// Number of taps for a Kaiser-windowed low-pass with the given transition
// width and stop-band attenuation (always odd).
SHARKSDR_API size_t kaiserTapCount(double transitionHz, double sampleRate, double attenuationDb = 70.0);

// Windowed-sinc low-pass. `cutoffHz` is the -6 dB point. DC gain is `gain`.
// If `taps` is 0 the length is derived from the transition width.
SHARKSDR_API std::vector<float> designLowpass(double cutoffHz, double transitionHz, double sampleRate,
                                              double attenuationDb = 70.0, size_t taps = 0, double gain = 1.0);

// Complex band-pass covering [lowHz, highHz] (may be asymmetric around 0).
SHARKSDR_API std::vector<Complex> designComplexBandpass(double lowHz, double highHz, double transitionHz,
                                                        double sampleRate, double attenuationDb = 70.0);

// ---- Dot products -------------------------------------------------------------

// The inner loops of every FIR filter, vectorised with SSE2 / NEON.
//   dotProduct       sum of x[i] * h[i]
//   dotProduct2      the same x against two tap sets at once
//   pairDotProduct   x holds n interleaved pairs (complex or stereo samples) and
//                    hh the taps written twice ([h0 h0 h1 h1 ...]); returns the
//                    sum for the first and for the second member of the pairs
//   pairDotProduct2  the same against two tap sets at once
SHARKSDR_API float dotProduct(const float* x, const float* h, size_t n);
SHARKSDR_API void dotProduct2(const float* x, const float* h0, const float* h1, size_t n, float* s0, float* s1);
SHARKSDR_API void pairDotProduct(const float* x, const float* hh, size_t n, float* first, float* second);
SHARKSDR_API void pairDotProduct2(const float* x, const float* hh0, const float* hh1, size_t n, float* first0, float* second0,
                                  float* first1, float* second1);

namespace detail {

// Complex and Stereo are two packed floats and use the pair kernels.
template <typename T>
constexpr bool kIsPair = std::is_same_v<T, Complex> || std::is_same_v<T, Stereo>;

inline std::vector<float> duplicateTaps(const float* h, size_t n) {
    std::vector<float> hh(2 * n);
    for (size_t i = 0; i < n; i++) hh[2 * i] = hh[2 * i + 1] = h[i];
    return hh;
}

template <typename T>
T dot(const T* x, const float* taps, const float* taps2, size_t n) {
    if constexpr (std::is_same_v<T, float>) {
        return dotProduct(x, taps, n);
    } else if constexpr (kIsPair<T>) {
        float a, b;
        pairDotProduct(reinterpret_cast<const float*>(x), taps2, n, &a, &b);
        return T(a, b);
    } else {
        T acc{};
        for (size_t k = 0; k < n; k++) acc += x[k] * taps[k];
        return acc;
    }
}

}  // namespace detail

// ---- FIR filters --------------------------------------------------------------

// Real taps applied to float, Complex or Stereo samples.
template <typename T>
class FirFilter {
public:
    FirFilter() = default;
    explicit FirFilter(const std::vector<float>& taps) { setTaps(taps); }

    void setTaps(const std::vector<float>& taps) {
        n_ = taps.size();
        taps_.assign(taps.rbegin(), taps.rend());  // oldest sample first
        if constexpr (detail::kIsPair<T>) taps2_ = detail::duplicateTaps(taps_.data(), n_);
        reset();
    }
    void reset() { buffer_.assign(history(), T{}); }
    size_t tapCount() const { return n_; }

    // In-place processing is allowed (out == in).
    void process(const T* in, size_t count, T* out) {
        if (n_ == 0) {
            if (out != in) std::copy(in, in + count, out);
            return;
        }
        if (count == 0) return;
        const size_t h = history();
        buffer_.resize(h + count);
        std::copy(in, in + count, buffer_.begin() + std::ptrdiff_t(h));
        for (size_t i = 0; i < count; i++) out[i] = detail::dot(&buffer_[i], taps_.data(), taps2_.data(), n_);
        std::copy(buffer_.end() - std::ptrdiff_t(h), buffer_.end(), buffer_.begin());
        buffer_.resize(h);
    }

private:
    size_t history() const { return n_ > 0 ? n_ - 1 : 0; }

    std::vector<float> taps_, taps2_;
    std::vector<T> buffer_;  // the last n-1 inputs, then the block being filtered
    size_t n_ = 0;
};

// Complex taps applied to complex data (asymmetric band-pass, e.g. SSB).
class SHARKSDR_API ComplexFirFilter {
public:
    void setTaps(const std::vector<Complex>& taps);
    void reset();
    size_t tapCount() const { return n_; }
    void process(const Complex* in, size_t count, Complex* out);  // in-place allowed

private:
    std::vector<float> re2_, im2_;  // real and imaginary parts, reversed and written twice
    std::vector<Complex> buffer_;
    size_t n_ = 0;
};

// FIR filter followed by integer decimation (only the kept outputs are computed).
class SHARKSDR_API FirDecimator {
public:
    void configure(const std::vector<float>& taps, int decimation);
    void reset();
    // Returns the number of output samples. In-place allowed.
    size_t process(const Complex* in, size_t count, Complex* out);

private:
    std::vector<float> taps2_;
    std::vector<Complex> buffer_;
    size_t n_ = 0;
    int decimation_ = 1;
    int phase_ = 0;
};

// Half-band filters. Every other tap of a half-band low-pass is zero, so the
// samples are split into even and odd ones and only the non-zero taps are
// computed. These work on pairs of floats (Complex or Stereo samples, see the
// typed wrappers below); `count` is the number of pairs.
class SHARKSDR_API HalfBandPairDecimator {
public:
    // Keeps [-passHz, passHz] of a signal sampled at `inRate` free of aliases.
    void configure(double passHz, double inRate, double attenuationDb);
    void reset();
    size_t tapCount() const { return 4 * k_ + 3; }
    // One output per odd input sample. In-place allowed.
    size_t process(const float* in, size_t count, float* out);

private:
    std::vector<float> taps2_;       // the taps at an odd distance from the centre, written twice
    std::vector<float> even_, odd_;  // history followed by the pairs of the current block
    float center_ = 0.5f;
    size_t k_ = 0;
    size_t parity_ = 0;  // 1 when the next input sample is an odd one
};

class SHARKSDR_API HalfBandPairInterpolator {
public:
    // Doubles the rate of a signal whose content is within [-passHz, passHz];
    // `outRate` is the new (doubled) rate.
    void configure(double passHz, double outRate, double attenuationDb);
    void reset();
    // Writes 2 * count pairs. `out` must not overlap `in`.
    void process(const float* in, size_t count, float* out);

private:
    std::vector<float> taps2_;
    std::vector<float> buffer_;  // history followed by the current block
    float center_ = 1.0f;
    size_t k_ = 0;
};

// Decimation by two with a half-band low-pass. T = Complex or Stereo.
template <typename T>
class HalfBandDecimator {
    static_assert(detail::kIsPair<T>, "HalfBandDecimator works on Complex or Stereo samples");

public:
    void configure(double passHz, double inRate, double attenuationDb = 100.0) { impl_.configure(passHz, inRate, attenuationDb); }
    void reset() { impl_.reset(); }
    size_t tapCount() const { return impl_.tapCount(); }
    // Returns the number of outputs (about count / 2). In-place allowed.
    size_t process(const T* in, size_t count, T* out) {
        return impl_.process(reinterpret_cast<const float*>(in), count, reinterpret_cast<float*>(out));
    }

private:
    HalfBandPairDecimator impl_;
};

// Interpolation by two with a half-band low-pass. T = Complex or Stereo.
template <typename T>
class HalfBandInterpolator {
    static_assert(detail::kIsPair<T>, "HalfBandInterpolator works on Complex or Stereo samples");

public:
    void configure(double passHz, double outRate, double attenuationDb = 90.0) { impl_.configure(passHz, outRate, attenuationDb); }
    void reset() { impl_.reset(); }
    // Writes 2 * count samples to `out` (which must not overlap `in`).
    void process(const T* in, size_t count, T* out) {
        impl_.process(reinterpret_cast<const float*>(in), count, reinterpret_cast<float*>(out));
    }

private:
    HalfBandPairInterpolator impl_;
};

// Cascade of half-band decimate-by-two stages. Each stage only protects the
// band that is needed at the very end, which keeps the early (fast) stages
// very short. T = Complex or Stereo.
template <typename T>
class DecimatorCascade {
public:
    // Decimate from `inRate` while the rate stays >= `minOutRate`; the band
    // [-passbandHz, passbandHz] is kept alias free.
    void configure(double inRate, double minOutRate, double passbandHz, double attenuationDb = 100.0) {
        stages_.clear();
        double rate = inRate;
        while (rate / 2.0 >= minOutRate) {
            const double pass = std::min(passbandHz, rate * 0.225);  // 0.45 of the output rate
            if (rate / 2.0 - 2.0 * pass <= 0.0) break;
            stages_.emplace_back();
            stages_.back().configure(pass, rate, attenuationDb);
            rate /= 2.0;
        }
        outRate_ = rate;
    }
    void reset() {
        for (auto& s : stages_) s.reset();
    }
    // Returns the number of output samples. In-place allowed.
    size_t process(const T* in, size_t count, T* out) {
        if (stages_.empty()) {
            if (out != in) std::copy(in, in + count, out);
            return count;
        }
        size_t n = stages_[0].process(in, count, out);
        for (size_t i = 1; i < stages_.size(); i++) n = stages_[i].process(out, n, out);
        return n;
    }

    double outputRate() const { return outRate_; }
    int factor() const { return 1 << int(stages_.size()); }

private:
    std::vector<HalfBandDecimator<T>> stages_;
    double outRate_ = 0.0;
};

using PowerDecimator = DecimatorCascade<Complex>;

// ---- Simple filters -------------------------------------------------------------

// Single pole DC blocker.
class DcBlocker {
public:
    void setRate(double alpha) { alpha_ = float(alpha); }
    void reset() { dc_ = {}; }
    void process(Complex* data, size_t count) {
        for (size_t i = 0; i < count; i++) {
            dc_ += (data[i] - dc_) * alpha_;
            data[i] -= dc_;
        }
    }

private:
    Complex dc_{};
    float alpha_ = 1e-5f;
};

// Automatic I/Q imbalance correction. A gain or phase error between the I and
// Q paths of a zero-IF tuner (E4000, FC0012/13, most direct conversion SDRs)
// mirrors every signal to the opposite side of the centre frequency, always
// with the same complex ratio b = Z(-f) / conj(Z(f)). update() measures that
// ratio on the spectrum bins where one side is far stronger than its mirror
// (stations that really sit on both sides are not mistaken for images) and
// process() removes it: z - b * conj(z). The spectrum is taken from the
// corrected signal, so the correction converges to no residual image.
//
// update() and process() may run on different threads.
class SHARKSDR_API IqBalancer {
public:
    void reset();
    // `bins`: n-point FFT of the corrected signal, natural order (DC first).
    void update(const Complex* bins, size_t n);
    void process(Complex* data, size_t count) const;
    // Current correction b (0 = balanced); the image rejection before it was
    // about -20 log10 |b| dB.
    Complex correction() const { return Complex(re_.load(), im_.load()); }

private:
    std::atomic<float> re_{0.0f}, im_{0.0f};
    std::vector<float> power_, scratch_;
};

// First order IIR low-pass, e.g. FM de-emphasis (tau in seconds).
class OnePole {
public:
    void configureTau(double tau, double sampleRate) {
        alpha_ = tau > 0.0 ? float(1.0 - std::exp(-1.0 / (tau * sampleRate))) : 1.0f;
    }
    void configureCutoff(double hz, double sampleRate) {
        alpha_ = float(1.0 - std::exp(-kTwoPi * hz / sampleRate));
    }
    void reset(float v = 0.0f) { y_ = v; }
    float process(float x) { return y_ += alpha_ * (x - y_); }
    float value() const { return y_; }

private:
    float alpha_ = 1.0f;
    float y_ = 0.0f;
};

}  // namespace SharkSDR::dsp
