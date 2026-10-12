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
                   double attenuationDb = 70.0, int phases = 64) {
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
        bank_.assign(size_t(phases + 1) * k, 0.0f);
        for (int p = 0; p <= phases; p++) {
            for (size_t j = 0; j < k; j++) {
                size_t idx = j * size_t(phases) + size_t(p);
                bank_[size_t(p) * k + (k - 1 - j)] = idx < protoLen ? proto[idx] : 0.0f;
            }
        }
        history_.assign(2 * k, T{});
        pos_ = 0;
        frac_ = 0.0;
    }

    void reset() {
        std::fill(history_.begin(), history_.end(), T{});
        pos_ = 0;
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
        size_t produced = 0;
        const size_t k = taps_;
        for (size_t i = 0; i < count; i++) {
            history_[pos_] = history_[pos_ + k] = in[i];
            pos_ = pos_ + 1 == k ? 0 : pos_ + 1;
            const T* x = &history_[pos_];
            while (frac_ < 1.0) {
                double ph = frac_ * phases_;
                int p = int(ph);
                float a = float(ph - p);
                const float* b0 = &bank_[size_t(p) * k];
                const float* b1 = b0 + k;
                if constexpr (sizeof(T) == 2 * sizeof(float)) {
                    // Complex / Stereo: two interleaved floats, written so the
                    // compiler can vectorise it.
                    const float* xf = reinterpret_cast<const float*>(x);
                    float r0 = 0.0f, i0 = 0.0f, r1 = 0.0f, i1 = 0.0f;
                    for (size_t j = 0; j < k; j++) {
                        const float xr = xf[2 * j], xi = xf[2 * j + 1];
                        r0 += xr * b0[j];
                        i0 += xi * b0[j];
                        r1 += xr * b1[j];
                        i1 += xi * b1[j];
                    }
                    const float wa = 1.0f - a;
                    T v;
                    float* vf = reinterpret_cast<float*>(&v);
                    vf[0] = r0 * wa + r1 * a;
                    vf[1] = i0 * wa + i1 * a;
                    out[produced++] = v;
                } else {
                    T acc0{}, acc1{};
                    for (size_t j = 0; j < k; j++) {
                        acc0 += x[j] * b0[j];
                        acc1 += x[j] * b1[j];
                    }
                    out[produced++] = acc0 * (1.0f - a) + acc1 * a;
                }
                frac_ += step_;
            }
            frac_ -= 1.0;
        }
        return produced;
    }

private:
    std::vector<float> bank_;
    std::vector<T> history_;
    size_t taps_ = 0;
    size_t pos_ = 0;
    int phases_ = 64;
    double step_ = 1.0;
    double frac_ = 0.0;
    double inRate_ = 0.0, outRate_ = 0.0;
    bool bypass_ = false;
};

}  // namespace SharkSDR::dsp
