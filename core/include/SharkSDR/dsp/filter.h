#pragma once

#include <SharkSDR/export.h>
#include <SharkSDR/types.h>

#include <algorithm>
#include <cmath>
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

// ---- FIR filters --------------------------------------------------------------

// Real taps applied to complex or real data (T = Complex or float).
template <typename T>
class FirFilter {
public:
    FirFilter() = default;
    explicit FirFilter(const std::vector<float>& taps) { setTaps(taps); }

    void setTaps(const std::vector<float>& taps) {
        n_ = taps.size();
        taps_.assign(taps.rbegin(), taps.rend());  // oldest sample first
        history_.assign(2 * n_, T{});
        pos_ = 0;
    }
    void reset() {
        std::fill(history_.begin(), history_.end(), T{});
        pos_ = 0;
    }
    size_t tapCount() const { return n_; }

    // In-place processing is allowed (out == in).
    void process(const T* in, size_t count, T* out) {
        if (n_ == 0) {
            if (out != in) std::copy(in, in + count, out);
            return;
        }
        for (size_t i = 0; i < count; i++) {
            history_[pos_] = history_[pos_ + n_] = in[i];
            pos_ = pos_ + 1 == n_ ? 0 : pos_ + 1;
            out[i] = dot(&history_[pos_]);
        }
    }

private:
    T dot(const T* x) const {
        T acc{};
        const float* t = taps_.data();
        for (size_t k = 0; k < n_; k++) acc += x[k] * t[k];
        return acc;
    }

    std::vector<float> taps_;
    std::vector<T> history_;
    size_t n_ = 0;
    size_t pos_ = 0;
};

// Complex taps applied to complex data (asymmetric band-pass, e.g. SSB).
class SHARKSDR_API ComplexFirFilter {
public:
    void setTaps(const std::vector<Complex>& taps);
    void reset();
    size_t tapCount() const { return n_; }
    void process(const Complex* in, size_t count, Complex* out);  // in-place allowed

private:
    std::vector<float> tapsRe_, tapsIm_;  // reversed
    std::vector<float> histRe_, histIm_;  // doubled history buffers
    size_t n_ = 0;
    size_t pos_ = 0;
};

// FIR filter followed by integer decimation (only the kept outputs are computed).
class SHARKSDR_API FirDecimator {
public:
    void configure(const std::vector<float>& taps, int decimation);
    void reset();
    // Returns the number of output samples. In-place allowed.
    size_t process(const Complex* in, size_t count, Complex* out);

private:
    std::vector<float> taps_;
    std::vector<Complex> history_;
    size_t n_ = 0;
    size_t pos_ = 0;
    int decimation_ = 1;
    int phase_ = 0;
};

// Cascade of decimate-by-two stages. Each stage only protects the band that
// is needed at the very end, which keeps the early (fast) stages very short.
class SHARKSDR_API PowerDecimator {
public:
    // Decimate from `inRate` while the rate stays >= `minOutRate`; the band
    // [-passbandHz, passbandHz] is kept alias free.
    void configure(double inRate, double minOutRate, double passbandHz);
    void reset();
    size_t process(const Complex* in, size_t count, Complex* out);  // in-place allowed

    double outputRate() const { return outRate_; }
    int factor() const { return 1 << int(stages_.size()); }

private:
    std::vector<FirDecimator> stages_;
    double outRate_ = 0.0;
};

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
