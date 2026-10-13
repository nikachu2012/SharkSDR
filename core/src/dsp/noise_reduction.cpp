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

// ---- AudioNoiseReduction ------------------------------------------------------------

namespace {

// Minimum statistics: the noise of a bin is the minimum of its smoothed power
// over kSubWindows sub-windows of kSubWindowSeconds (~1.5 s in all).
constexpr size_t kSubWindows = 8;
constexpr double kSubWindowSeconds = 0.19;
constexpr float kSmoothing = 0.85f;  // power smoothing before the minimum search
constexpr float kBias = 2.0f;        // that minimum -> mean noise power
// The noise estimate is a median over neighbouring bins, so that steady tones
// (a held note, a CW carrier) are not mistaken for noise.
constexpr size_t kMedianBins = 11;
constexpr float kDecisionDirected = 0.98f;
constexpr float kMinPrioriSnr = 0.003f;  // -25 dB

// Exponential integral E1(x) for x > 0 (Abramowitz & Stegun 5.1.53 / 5.1.56).
float expint(float x) {
    if (x <= 1.0f)
        return -0.57721566f - std::log(x) +
               x * (0.99999193f + x * (-0.24991055f + x * (0.05519968f + x * (-0.00976004f + x * 0.00107857f))));
    if (x > 30.0f) return 0.0f;
    return std::exp(-x) / x * (x * x + 2.334733f * x + 0.250621f) / (x * x + 3.330657f * x + 1.681534f);
}

}  // namespace

void AudioNoiseReduction::configure(double sampleRate, float reductionDb) {
    n_ = 256;
    while (double(n_) < sampleRate * 0.02) n_ <<= 1;  // 1024 at 48 kHz
    hop_ = n_ / 4;
    bins_ = n_ / 2 + 1;
    fft_.resize(n_);
    window_.resize(n_);
    for (size_t i = 0; i < n_; i++) window_[i] = float(0.5 - 0.5 * std::cos(kTwoPi * double(i) / double(n_)));
    work_.resize(n_);
    subLength_ = std::max<size_t>(1, size_t(std::lround(kSubWindowSeconds * sampleRate / double(hop_))));
    setReduction(reductionDb);
    reset();
}

void AudioNoiseReduction::setReduction(float reductionDb) { floor_ = std::pow(10.0f, -std::max(reductionDb, 0.0f) / 20.0f); }

void AudioNoiseReduction::reset() {
    frame_.assign(n_, Complex());
    sum_.assign(n_, Complex());
    ready_.assign(hop_, Complex());
    fill_ = 0;
    smooth_.assign(bins_, 0.0f);
    subMin_.assign(bins_, 1e30f);
    minima_.assign(kSubWindows * bins_, 1e30f);
    cleanSnr_.assign(bins_, 0.0f);
    noise_.assign(bins_, 0.0f);
    gain_.assign(bins_, 1.0f);
    frameInSub_ = subIndex_ = frames_ = 0;
}

void AudioNoiseReduction::process(Stereo* data, size_t count) {
    if (n_ == 0) return;
    for (size_t i = 0; i < count; i++) {
        const Complex y = ready_[fill_];
        frame_[n_ - hop_ + fill_] = Complex(data[i].l, data[i].r);
        data[i] = Stereo(y.real(), y.imag());
        if (++fill_ == hop_) {
            processFrame();
            fill_ = 0;
        }
    }
}

void AudioNoiseReduction::processFrame() {
    // Left and right in one complex FFT: Z = L + jR.
    for (size_t k = 0; k < n_; k++) work_[k] = frame_[k] * window_[k];
    fft_.forward(work_.data());

    // Power per bin (|L|^2 + |R|^2 from Z(k) and Z(n-k)), and its running minimum.
    for (size_t k = 0; k < bins_; k++) {
        const float p = 0.5f * (magSquared(work_[k]) + magSquared(work_[(n_ - k) & (n_ - 1)])) + 1e-30f;
        smooth_[k] = frames_ == 0 ? p : kSmoothing * smooth_[k] + (1.0f - kSmoothing) * p;
        subMin_[k] = std::min(subMin_[k], smooth_[k]);
        float m = subMin_[k];
        for (size_t u = 0; u < kSubWindows; u++) m = std::min(m, minima_[u * bins_ + k]);
        noise_[k] = m;
    }
    if (++frameInSub_ == subLength_) {
        std::copy(subMin_.begin(), subMin_.end(), minima_.begin() + std::ptrdiff_t(subIndex_ * bins_));
        std::fill(subMin_.begin(), subMin_.end(), 1e30f);
        subIndex_ = (subIndex_ + 1) % kSubWindows;
        frameInSub_ = 0;
    }

    for (size_t k = 0; k < bins_; k++) {
        // Noise: median of the minima around this bin.
        const size_t lo = k >= kMedianBins / 2 ? k - kMedianBins / 2 : 0;
        const size_t hi = std::min(bins_, lo + kMedianBins);
        scratch_.assign(noise_.begin() + std::ptrdiff_t(lo), noise_.begin() + std::ptrdiff_t(hi));
        std::nth_element(scratch_.begin(), scratch_.begin() + std::ptrdiff_t(scratch_.size() / 2), scratch_.end());
        const float noise = kBias * scratch_[scratch_.size() / 2];

        const float p = 0.5f * (magSquared(work_[k]) + magSquared(work_[(n_ - k) & (n_ - 1)])) + 1e-30f;
        const float gamma = std::min(p / noise, 1e4f);  // a-posteriori SNR
        const float ml = std::max(gamma - 1.0f, 0.0f);
        float xi = frames_ == 0 ? ml : kDecisionDirected * cleanSnr_[k] + (1.0f - kDecisionDirected) * ml;
        xi = std::max(xi, kMinPrioriSnr);  // a-priori SNR
        const float w = xi / (1.0f + xi);
        float g = w * std::exp(0.5f * expint(std::max(w * gamma, 1e-6f)));  // MMSE-LSA
        g = std::clamp(g, floor_, 1.0f);
        cleanSnr_[k] = g * g * gamma;
        gain_[k] = g;
    }
    frames_++;

    work_[0] *= gain_[0];
    work_[n_ / 2] *= gain_[n_ / 2];
    for (size_t k = 1; k < n_ / 2; k++) {
        work_[k] *= gain_[k];
        work_[n_ - k] *= gain_[k];
    }

    // Inverse FFT via the conjugate trick, synthesis window, overlap-add
    // (Hann^2 at 75 % overlap sums to 1.5).
    for (auto& v : work_) v = std::conj(v);
    fft_.forward(work_.data());
    const float scale = 1.0f / (float(n_) * 1.5f);
    for (size_t k = 0; k < n_; k++) sum_[k] += std::conj(work_[k]) * (scale * window_[k]);
    std::copy(sum_.begin(), sum_.begin() + std::ptrdiff_t(hop_), ready_.begin());
    std::copy(sum_.begin() + std::ptrdiff_t(hop_), sum_.end(), sum_.begin());
    std::fill(sum_.end() - std::ptrdiff_t(hop_), sum_.end(), Complex());
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
