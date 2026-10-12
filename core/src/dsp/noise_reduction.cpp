#include <SharkSDR/dsp/noise_reduction.h>

#include <algorithm>
#include <cmath>

namespace SharkSDR::dsp {

// ---- FmIfNoiseReduction -----------------------------------------------------------

void FmIfNoiseReduction::configure(size_t fftSize, size_t keepBins) {
    n_ = std::max<size_t>(fftSize, 8);
    hop_ = n_ / 2;
    keep_ = keepBins;
    fft_.resize(n_);
    // Periodic sqrt-Hann: analysis * synthesis windows overlap-add to exactly 1 at 50 % overlap.
    window_.resize(n_);
    for (size_t i = 0; i < n_; i++) window_[i] = float(std::sin(kPi * double(i) / double(n_)));
    work_.resize(n_);
    power_.resize(n_);
    reset();
}

void FmIfNoiseReduction::reset() {
    frame_.assign(n_, Complex());
    tail_.assign(hop_, Complex());
    ready_.assign(hop_, Complex());
    fill_ = 0;
}

void FmIfNoiseReduction::process(const Complex* in, size_t count, Complex* out) {
    if (n_ == 0) {
        if (out != in) std::copy(in, in + count, out);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        const Complex x = in[i];
        out[i] = ready_[fill_];
        frame_[hop_ + fill_] = x;
        if (++fill_ == hop_) {
            processFrame();
            fill_ = 0;
        }
    }
}

void FmIfNoiseReduction::processFrame() {
    for (size_t k = 0; k < n_; k++) work_[k] = frame_[k] * window_[k];
    fft_.forward(work_.data());

    // Strongest bin, judged on the power of 3 neighbouring bins.
    for (size_t k = 0; k < n_; k++) power_[k] = magSquared(work_[k]);
    size_t best = 0;
    float bestPower = -1.0f;
    for (size_t k = 0; k < n_; k++) {
        const float p = power_[(k + n_ - 1) % n_] + power_[k] + power_[(k + 1) % n_];
        if (p > bestPower) {
            bestPower = p;
            best = k;
        }
    }

    // Keep +-keep_ bins around it (with a half-weight bin on each edge).
    for (size_t k = 0; k < n_; k++) {
        const size_t a = k > best ? k - best : best - k;
        const size_t d = std::min(a, n_ - a);
        if (d > keep_ + 1) work_[k] = Complex();
        else if (d == keep_ + 1) work_[k] *= 0.5f;
    }

    // Inverse FFT via the conjugate trick, synthesis window, overlap-add.
    for (auto& v : work_) v = std::conj(v);
    fft_.forward(work_.data());
    const float scale = 1.0f / float(n_);
    for (size_t k = 0; k < hop_; k++) {
        const Complex y0 = std::conj(work_[k]) * (scale * window_[k]);
        const Complex y1 = std::conj(work_[k + hop_]) * (scale * window_[k + hop_]);
        ready_[k] = y0 + tail_[k];
        tail_[k] = y1;
    }

    std::copy(frame_.begin() + std::ptrdiff_t(hop_), frame_.end(), frame_.begin());
}

// ---- Biquad / Butterworth -----------------------------------------------------------

void Biquad::design(double cutoffHz, double q, double sampleRate, bool highpass) {
    const double f = std::clamp(cutoffHz, 1.0, sampleRate * 0.49);
    const double w0 = kTwoPi * f / sampleRate;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    if (highpass) {
        b0_ = (1.0 + cw) / 2.0 / a0;
        b1_ = -(1.0 + cw) / a0;
    } else {
        b0_ = (1.0 - cw) / 2.0 / a0;
        b1_ = (1.0 - cw) / a0;
    }
    b2_ = b0_;
    a1_ = -2.0 * cw / a0;
    a2_ = (1.0 - alpha) / a0;
}

void ButterworthFilter::configure(bool highpass, double cutoffHz, double sampleRate, int order) {
    const int sections = std::max(1, order / 2);
    left_.assign(size_t(sections), Biquad());
    right_.assign(size_t(sections), Biquad());
    for (int k = 0; k < sections; k++) {
        const double q = 1.0 / (2.0 * std::cos(kPi * double(2 * k + 1) / double(4 * sections)));
        if (highpass) {
            left_[size_t(k)].setHighpass(cutoffHz, q, sampleRate);
            right_[size_t(k)].setHighpass(cutoffHz, q, sampleRate);
        } else {
            left_[size_t(k)].setLowpass(cutoffHz, q, sampleRate);
            right_[size_t(k)].setLowpass(cutoffHz, q, sampleRate);
        }
    }
}

void ButterworthFilter::reset() {
    for (auto& b : left_) b.reset();
    for (auto& b : right_) b.reset();
}

void ButterworthFilter::process(Stereo* data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        float l = data[i].l, r = data[i].r;
        for (auto& b : left_) l = b.process(l);
        for (auto& b : right_) r = b.process(r);
        data[i] = Stereo(l, r);
    }
}

}  // namespace SharkSDR::dsp
