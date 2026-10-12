#include <SharkSDR/dsp/filter.h>
#include <SharkSDR/dsp/window.h>

#include <cmath>

namespace SharkSDR::dsp {

namespace {
double kaiserBeta(double attenuationDb) {
    if (attenuationDb > 50.0) return 0.1102 * (attenuationDb - 8.7);
    if (attenuationDb > 21.0) return 0.5842 * std::pow(attenuationDb - 21.0, 0.4) + 0.07886 * (attenuationDb - 21.0);
    return 0.0;
}
}  // namespace

size_t kaiserTapCount(double transitionHz, double sampleRate, double attenuationDb) {
    double dw = kTwoPi * std::max(transitionHz, 1e-9) / sampleRate;
    size_t n = size_t(std::ceil((attenuationDb - 8.0) / (2.285 * dw))) + 1;
    n = std::clamp<size_t>(n, 3, 1 << 16);
    return n | 1;
}

std::vector<float> designLowpass(double cutoffHz, double transitionHz, double sampleRate,
                                 double attenuationDb, size_t taps, double gain) {
    if (taps == 0) taps = kaiserTapCount(transitionHz, sampleRate, attenuationDb);
    std::vector<float> window = kaiserWindow(taps, kaiserBeta(attenuationDb));
    std::vector<float> h(taps);
    const double fc = cutoffHz / sampleRate;
    const double mid = double(taps - 1) / 2.0;
    double sum = 0.0;
    for (size_t i = 0; i < taps; i++) {
        double t = double(i) - mid;
        double s = std::abs(t) < 1e-9 ? 2.0 * fc : std::sin(kTwoPi * fc * t) / (kPi * t);
        h[i] = float(s * window[i]);
        sum += h[i];
    }
    if (std::abs(sum) > 1e-12)
        for (auto& v : h) v = float(v * gain / sum);
    return h;
}

std::vector<Complex> designComplexBandpass(double lowHz, double highHz, double transitionHz,
                                           double sampleRate, double attenuationDb) {
    const double center = 0.5 * (lowHz + highHz);
    const double half = 0.5 * std::abs(highHz - lowHz);
    std::vector<float> lp = designLowpass(half, transitionHz, sampleRate, attenuationDb);
    std::vector<Complex> h(lp.size());
    const double mid = double(lp.size() - 1) / 2.0;
    for (size_t i = 0; i < lp.size(); i++) {
        double ph = kTwoPi * center * (double(i) - mid) / sampleRate;
        h[i] = Complex(float(lp[i] * std::cos(ph)), float(lp[i] * std::sin(ph)));
    }
    return h;
}

// ---- ComplexFirFilter ---------------------------------------------------------

void ComplexFirFilter::setTaps(const std::vector<Complex>& taps) {
    n_ = taps.size();
    tapsRe_.resize(n_);
    tapsIm_.resize(n_);
    for (size_t i = 0; i < n_; i++) {
        tapsRe_[n_ - 1 - i] = taps[i].real();
        tapsIm_[n_ - 1 - i] = taps[i].imag();
    }
    histRe_.assign(2 * n_, 0.0f);
    histIm_.assign(2 * n_, 0.0f);
    pos_ = 0;
}

void ComplexFirFilter::reset() {
    std::fill(histRe_.begin(), histRe_.end(), 0.0f);
    std::fill(histIm_.begin(), histIm_.end(), 0.0f);
    pos_ = 0;
}

void ComplexFirFilter::process(const Complex* in, size_t count, Complex* out) {
    if (n_ == 0) {
        if (out != in) std::copy(in, in + count, out);
        return;
    }
    const float* hr = tapsRe_.data();
    const float* hi = tapsIm_.data();
    for (size_t i = 0; i < count; i++) {
        histRe_[pos_] = histRe_[pos_ + n_] = in[i].real();
        histIm_[pos_] = histIm_[pos_ + n_] = in[i].imag();
        pos_ = pos_ + 1 == n_ ? 0 : pos_ + 1;
        const float* xr = &histRe_[pos_];
        const float* xi = &histIm_[pos_];
        float rr = 0.0f, ii = 0.0f, ri = 0.0f, ir = 0.0f;
        for (size_t k = 0; k < n_; k++) {
            rr += xr[k] * hr[k];
            ii += xi[k] * hi[k];
            ri += xr[k] * hi[k];
            ir += xi[k] * hr[k];
        }
        out[i] = Complex(rr - ii, ri + ir);
    }
}

// ---- FirDecimator ---------------------------------------------------------------

void FirDecimator::configure(const std::vector<float>& taps, int decimation) {
    n_ = taps.size();
    taps_.assign(taps.rbegin(), taps.rend());
    history_.assign(2 * n_, Complex{});
    decimation_ = std::max(1, decimation);
    pos_ = 0;
    phase_ = 0;
}

void FirDecimator::reset() {
    std::fill(history_.begin(), history_.end(), Complex{});
    pos_ = 0;
    phase_ = 0;
}

size_t FirDecimator::process(const Complex* in, size_t count, Complex* out) {
    size_t produced = 0;
    const float* t = taps_.data();
    for (size_t i = 0; i < count; i++) {
        history_[pos_] = history_[pos_ + n_] = in[i];
        pos_ = pos_ + 1 == n_ ? 0 : pos_ + 1;
        if (++phase_ < decimation_) continue;
        phase_ = 0;
        const Complex* x = &history_[pos_];
        float re = 0.0f, im = 0.0f;
        for (size_t k = 0; k < n_; k++) {
            re += x[k].real() * t[k];
            im += x[k].imag() * t[k];
        }
        out[produced++] = Complex(re, im);
    }
    return produced;
}

// ---- PowerDecimator -------------------------------------------------------------

void PowerDecimator::configure(double inRate, double minOutRate, double passbandHz) {
    stages_.clear();
    double rate = inRate;
    while (rate / 2.0 >= minOutRate) {
        const double outRate = rate / 2.0;
        const double pass = std::min(passbandHz, outRate * 0.45);
        const double stopEdge = outRate - pass;  // energy above this folds outside +-pass
        const double transition = stopEdge - pass;
        if (transition <= 0.0) break;
        FirDecimator stage;
        stage.configure(designLowpass(outRate / 2.0, transition, rate, 70.0), 2);
        stages_.push_back(std::move(stage));
        rate = outRate;
    }
    outRate_ = rate;
}

void PowerDecimator::reset() {
    for (auto& s : stages_) s.reset();
}

size_t PowerDecimator::process(const Complex* in, size_t count, Complex* out) {
    if (stages_.empty()) {
        if (out != in) std::copy(in, in + count, out);
        return count;
    }
    size_t n = stages_[0].process(in, count, out);
    for (size_t i = 1; i < stages_.size(); i++) n = stages_[i].process(out, n, out);
    return n;
}

}  // namespace SharkSDR::dsp
