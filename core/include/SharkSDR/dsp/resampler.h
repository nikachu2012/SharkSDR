#pragma once

#include <SharkSDR/dsp/filter.h>
#include <SharkSDR/types.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace SharkSDR::dsp {

// Arbitrary ratio polyphase resampler with a built-in anti-alias/anti-image
// low-pass. Works for both decimation and interpolation; the output time is
// interpolated linearly between adjacent filter phases.
//
// T can be Complex, float or Stereo.
template <typename T>
class Resampler {
public:
    // cutoffHz: -6 dB point of the low-pass (should be below min(in,out)/2)
    // transitionHz: transition band width
    void configure(double inRate, double outRate, double cutoffHz, double transitionHz,
                   double attenuationDb = 80.0, int phases = 128) {
        inRate_ = inRate;
        outRate_ = outRate;
        step_ = inRate / outRate;
        phases_ = phases;
        bypass_ = std::abs(inRate - outRate) < 1e-6 && cutoffHz >= inRate * 0.5;

        // Taps per phase, designed at the input rate.
        size_t k = kaiserTapCount(transitionHz, inRate, attenuationDb);
        k = std::max<size_t>(k, 4);
        taps_ = k;

        // Prototype at inRate * phases, one extra sample for phase interpolation.
        const size_t protoLen = k * size_t(phases) + 1;
        std::vector<float> proto = designLowpass(cutoffHz, transitionHz, inRate * phases,
                                                 attenuationDb, protoLen, double(phases));

        // bank_[p][j] = proto[(k-1-j)*phases + p]  -> history is oldest first.
        // Complex / Stereo samples use the taps written twice (pair kernels).
        const size_t copies = detail::kIsPair<T> ? 2 : 1;
        bank_.assign(size_t(phases + 1) * k * copies, 0.0f);
        for (int p = 0; p <= phases; p++) {
            for (size_t j = 0; j < k; j++) {
                const size_t idx = j * size_t(phases) + size_t(p);
                const float v = idx < protoLen ? proto[idx] : 0.0f;
                float* dst = &bank_[(size_t(p) * k + (k - 1 - j)) * copies];
                for (size_t c = 0; c < copies; c++) dst[c] = v;
            }
        }
        reset();
    }

    void reset() {
        buffer_.assign(taps_ > 0 ? taps_ - 1 : 0, T{});
        frac_ = 0.0;
    }

    double inputRate() const { return inRate_; }
    double outputRate() const { return outRate_; }

    // Upper bound of the output size for `count` input samples.
    size_t maxOutput(size_t count) const { return size_t(double(count) / step_) + 2; }

    // Returns the number of samples written to `out` (capacity >= maxOutput(count)).
    size_t process(const T* in, size_t count, T* out) {
        if (bypass_) {
            std::copy(in, in + count, out);
            return count;
        }
        if (count == 0) return 0;
        const size_t k = taps_, h = k - 1;
        buffer_.resize(h + count);
        std::copy(in, in + count, buffer_.begin() + std::ptrdiff_t(h));

        size_t produced = 0, i = 0;
        while (i < count) {
            if (frac_ >= 1.0) {
                // No output before the next input sample(s).
                const size_t skip = std::min(count - i, size_t(frac_));
                i += skip;
                frac_ -= double(skip);
                continue;
            }
            // Output between filter phases p and p + 1, window ending at input i.
            const double ph = frac_ * phases_;
            const int p = std::min(int(ph), phases_ - 1);
            const float a = float(ph - p);
            out[produced++] = interpolate(&buffer_[i], p, a);
            frac_ += step_;
        }
        std::copy(buffer_.end() - std::ptrdiff_t(h), buffer_.end(), buffer_.begin());
        buffer_.resize(h);
        return produced;
    }

private:
    T interpolate(const T* x, int p, float a) const {
        const size_t k = taps_;
        if constexpr (detail::kIsPair<T>) {
            const float* b0 = &bank_[size_t(p) * 2 * k];
            float f0, s0, f1, s1;
            pairDotProduct2(reinterpret_cast<const float*>(x), b0, b0 + 2 * k, k, &f0, &s0, &f1, &s1);
            return T(f0 + a * (f1 - f0), s0 + a * (s1 - s0));
        } else if constexpr (std::is_same_v<T, float>) {
            const float* b0 = &bank_[size_t(p) * k];
            float s0, s1;
            dotProduct2(x, b0, b0 + k, k, &s0, &s1);
            return s0 + a * (s1 - s0);
        } else {
            const float* b0 = &bank_[size_t(p) * k];
            const float* b1 = b0 + k;
            T acc0{}, acc1{};
            for (size_t j = 0; j < k; j++) {
                acc0 += x[j] * b0[j];
                acc1 += x[j] * b1[j];
            }
            return acc0 * (1.0f - a) + acc1 * a;
        }
    }

    std::vector<float> bank_;
    std::vector<T> buffer_;  // the last taps-1 inputs, then the current block
    size_t taps_ = 0;
    int phases_ = 128;
    double step_ = 1.0;
    double frac_ = 0.0;
    double inRate_ = 0.0, outRate_ = 0.0;
    bool bypass_ = false;
};

}  // namespace SharkSDR::dsp
