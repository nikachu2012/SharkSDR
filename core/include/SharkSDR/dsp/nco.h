#pragma once

#include <SharkSDR/types.h>

#include <cmath>

namespace SharkSDR::dsp {

// Numerically controlled oscillator implemented as a rotating phasor.
// mix() multiplies a signal by exp(j*2*pi*f*t), i.e. shifts it up by f.
class Nco {
public:
    void setFrequency(double hz, double sampleRate) {
        if (hz == freq_ && sampleRate == rate_) return;
        freq_ = hz;
        rate_ = sampleRate;
        double w = sampleRate > 0.0 ? kTwoPi * hz / sampleRate : 0.0;
        step_ = Complex(float(std::cos(w)), float(std::sin(w)));
        step4_ = Complex(float(std::cos(4.0 * w)), float(std::sin(4.0 * w)));
    }
    double frequency() const { return freq_; }

    void mix(const Complex* in, size_t count, Complex* out) {
        // Four phasors one sample apart, each advanced by four steps: the
        // multiplications no longer wait for each other.
        Complex p0 = phase_;
        Complex p1 = cmul(p0, step_);
        Complex p2 = cmul(p1, step_);
        Complex p3 = cmul(p2, step_);
        size_t i = 0;
        for (; i + 4 <= count; i += 4) {
            out[i] = cmul(in[i], p0);
            out[i + 1] = cmul(in[i + 1], p1);
            out[i + 2] = cmul(in[i + 2], p2);
            out[i + 3] = cmul(in[i + 3], p3);
            p0 = cmul(p0, step4_);
            p1 = cmul(p1, step4_);
            p2 = cmul(p2, step4_);
            p3 = cmul(p3, step4_);
        }
        Complex ph = p0;
        for (; i < count; i++) {
            out[i] = cmul(in[i], ph);
            ph = cmul(ph, step_);
        }
        // Renormalise to avoid amplitude drift.
        float mag = std::sqrt(magSquared(ph));
        phase_ = mag > 0.0f ? ph / mag : Complex(1.0f, 0.0f);
    }

    Complex next() {
        Complex r = phase_;
        phase_ = cmul(phase_, step_);
        if (++counter_ >= 4096) {
            counter_ = 0;
            phase_ /= std::sqrt(magSquared(phase_));
        }
        return r;
    }

    void reset() { phase_ = Complex(1.0f, 0.0f); }

private:
    Complex phase_{1.0f, 0.0f};
    Complex step_{1.0f, 0.0f};
    Complex step4_{1.0f, 0.0f};
    double freq_ = 0.0;
    double rate_ = 0.0;
    int counter_ = 0;
};

}  // namespace SharkSDR::dsp
