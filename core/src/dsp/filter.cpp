#include <SharkSDR/dsp/filter.h>
#include <SharkSDR/dsp/window.h>

#include <cmath>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#  define SHARKSDR_SSE2 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#  include <arm_neon.h>
#  define SHARKSDR_NEON 1
#endif

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
    std::vector<double> h(taps);
    const double fc = cutoffHz / sampleRate;
    const double mid = double(taps - 1) / 2.0;
    double sum = 0.0;
    for (size_t i = 0; i < taps; i++) {
        double t = double(i) - mid;
        double s = std::abs(t) < 1e-9 ? 2.0 * fc : std::sin(kTwoPi * fc * t) / (kPi * t);
        h[i] = s * window[i];
        sum += h[i];
    }
    std::vector<float> out(taps);
    const double scale = std::abs(sum) > 1e-12 ? gain / sum : 1.0;
    for (size_t i = 0; i < taps; i++) out[i] = float(h[i] * scale);
    return out;
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

// ---- Dot products -------------------------------------------------------------------

namespace {

#if SHARKSDR_SSE2
using Vec = __m128;
inline Vec vzero() { return _mm_setzero_ps(); }
inline Vec vload(const float* p) { return _mm_loadu_ps(p); }
inline Vec vmuladd(Vec acc, Vec a, Vec b) { return _mm_add_ps(acc, _mm_mul_ps(a, b)); }
inline Vec vadd(Vec a, Vec b) { return _mm_add_ps(a, b); }
inline void vstore(float* p, Vec v) { _mm_storeu_ps(p, v); }
#elif SHARKSDR_NEON
using Vec = float32x4_t;
inline Vec vzero() { return vdupq_n_f32(0.0f); }
inline Vec vload(const float* p) { return vld1q_f32(p); }
inline Vec vmuladd(Vec acc, Vec a, Vec b) { return vmlaq_f32(acc, a, b); }
inline Vec vadd(Vec a, Vec b) { return vaddq_f32(a, b); }
inline void vstore(float* p, Vec v) { vst1q_f32(p, v); }
#else
struct Vec {
    float v[4];
};
inline Vec vzero() { return {{0.0f, 0.0f, 0.0f, 0.0f}}; }
inline Vec vload(const float* p) { return {{p[0], p[1], p[2], p[3]}}; }
inline Vec vmuladd(Vec acc, Vec a, Vec b) {
    for (int i = 0; i < 4; i++) acc.v[i] += a.v[i] * b.v[i];
    return acc;
}
inline Vec vadd(Vec a, Vec b) {
    for (int i = 0; i < 4; i++) a.v[i] += b.v[i];
    return a;
}
inline void vstore(float* p, Vec v) {
    for (int i = 0; i < 4; i++) p[i] = v.v[i];
}
#endif

// lanes[j] = sum of x[i] * h[i] over the i < m with i % 4 == j.
inline void laneDot(const float* x, const float* h, size_t m, float lanes[4]) {
    Vec a0 = vzero(), a1 = vzero(), a2 = vzero(), a3 = vzero();
    size_t i = 0;
    for (; i + 16 <= m; i += 16) {
        a0 = vmuladd(a0, vload(x + i), vload(h + i));
        a1 = vmuladd(a1, vload(x + i + 4), vload(h + i + 4));
        a2 = vmuladd(a2, vload(x + i + 8), vload(h + i + 8));
        a3 = vmuladd(a3, vload(x + i + 12), vload(h + i + 12));
    }
    for (; i + 4 <= m; i += 4) a0 = vmuladd(a0, vload(x + i), vload(h + i));
    vstore(lanes, vadd(vadd(a0, a1), vadd(a2, a3)));
    for (size_t j = 0; i < m; i++, j++) lanes[j] += x[i] * h[i];
}

// The same for two tap sets, reading x once.
inline void laneDot2(const float* x, const float* h0, const float* h1, size_t m, float lanes0[4], float lanes1[4]) {
    Vec a0 = vzero(), a1 = vzero(), b0 = vzero(), b1 = vzero();
    size_t i = 0;
    for (; i + 8 <= m; i += 8) {
        const Vec x0 = vload(x + i), x1 = vload(x + i + 4);
        a0 = vmuladd(a0, x0, vload(h0 + i));
        a1 = vmuladd(a1, x1, vload(h0 + i + 4));
        b0 = vmuladd(b0, x0, vload(h1 + i));
        b1 = vmuladd(b1, x1, vload(h1 + i + 4));
    }
    for (; i + 4 <= m; i += 4) {
        const Vec x0 = vload(x + i);
        a0 = vmuladd(a0, x0, vload(h0 + i));
        b0 = vmuladd(b0, x0, vload(h1 + i));
    }
    vstore(lanes0, vadd(a0, a1));
    vstore(lanes1, vadd(b0, b1));
    for (size_t j = 0; i < m; i++, j++) {
        lanes0[j] += x[i] * h0[i];
        lanes1[j] += x[i] * h1[i];
    }
}

}  // namespace

float dotProduct(const float* x, const float* h, size_t n) {
    float l[4];
    laneDot(x, h, n, l);
    return (l[0] + l[1]) + (l[2] + l[3]);
}

void dotProduct2(const float* x, const float* h0, const float* h1, size_t n, float* s0, float* s1) {
    float l0[4], l1[4];
    laneDot2(x, h0, h1, n, l0, l1);
    *s0 = (l0[0] + l0[1]) + (l0[2] + l0[3]);
    *s1 = (l1[0] + l1[1]) + (l1[2] + l1[3]);
}

void pairDotProduct(const float* x, const float* hh, size_t n, float* first, float* second) {
    float l[4];
    laneDot(x, hh, 2 * n, l);
    *first = l[0] + l[2];
    *second = l[1] + l[3];
}

void pairDotProduct2(const float* x, const float* hh0, const float* hh1, size_t n, float* first0, float* second0,
                     float* first1, float* second1) {
    float l0[4], l1[4];
    laneDot2(x, hh0, hh1, 2 * n, l0, l1);
    *first0 = l0[0] + l0[2];
    *second0 = l0[1] + l0[3];
    *first1 = l1[0] + l1[2];
    *second1 = l1[1] + l1[3];
}

// ---- ComplexFirFilter ---------------------------------------------------------

void ComplexFirFilter::setTaps(const std::vector<Complex>& taps) {
    n_ = taps.size();
    re2_.resize(2 * n_);
    im2_.resize(2 * n_);
    for (size_t i = 0; i < n_; i++) {
        const Complex& t = taps[n_ - 1 - i];  // oldest sample first
        re2_[2 * i] = re2_[2 * i + 1] = t.real();
        im2_[2 * i] = im2_[2 * i + 1] = t.imag();
    }
    reset();
}

void ComplexFirFilter::reset() { buffer_.assign(n_ > 0 ? n_ - 1 : 0, Complex()); }

void ComplexFirFilter::process(const Complex* in, size_t count, Complex* out) {
    if (n_ == 0) {
        if (out != in) std::copy(in, in + count, out);
        return;
    }
    if (count == 0) return;
    const size_t h = n_ - 1;
    buffer_.resize(h + count);
    std::copy(in, in + count, buffer_.begin() + std::ptrdiff_t(h));
    for (size_t i = 0; i < count; i++) {
        // (xr + j xi)(hr + j hi): A = sum x*hr, B = sum x*hi (both lanes)
        float ar, ai, br, bi;
        pairDotProduct2(reinterpret_cast<const float*>(&buffer_[i]), re2_.data(), im2_.data(), n_, &ar, &ai, &br, &bi);
        out[i] = Complex(ar - bi, ai + br);
    }
    std::copy(buffer_.end() - std::ptrdiff_t(h), buffer_.end(), buffer_.begin());
    buffer_.resize(h);
}

// ---- FirDecimator ---------------------------------------------------------------

void FirDecimator::configure(const std::vector<float>& taps, int decimation) {
    n_ = taps.size();
    std::vector<float> reversed(taps.rbegin(), taps.rend());
    taps2_ = detail::duplicateTaps(reversed.data(), n_);
    decimation_ = std::max(1, decimation);
    reset();
}

void FirDecimator::reset() {
    buffer_.assign(n_ > 0 ? n_ - 1 : 0, Complex());
    phase_ = 0;
}

size_t FirDecimator::process(const Complex* in, size_t count, Complex* out) {
    if (n_ == 0 || count == 0) return 0;
    const size_t h = n_ - 1;
    buffer_.resize(h + count);
    std::copy(in, in + count, buffer_.begin() + std::ptrdiff_t(h));
    size_t produced = 0;
    for (size_t i = 0; i < count; i++) {
        if (++phase_ < decimation_) continue;
        phase_ = 0;
        float re, im;
        pairDotProduct(reinterpret_cast<const float*>(&buffer_[i]), taps2_.data(), n_, &re, &im);
        out[produced++] = Complex(re, im);
    }
    std::copy(buffer_.end() - std::ptrdiff_t(h), buffer_.end(), buffer_.begin());
    buffer_.resize(h);
    return produced;
}

// ---- Half-band filters ------------------------------------------------------------

namespace {

// Half-band low-pass of length 4k+3 (>= the Kaiser estimate, so that the
// outermost taps are not zero). Returns k; `odd` receives the taps at an odd
// distance from the centre (the even indices), `center` the centre tap.
size_t designHalfBand(double transition, double rate, double attenuationDb, std::vector<float>& odd, float& center) {
    const size_t k = kaiserTapCount(transition, rate, attenuationDb) / 4;
    const size_t n = 4 * k + 3;
    const std::vector<float> h = designLowpass(rate / 4.0, transition, rate, attenuationDb, n);
    odd.clear();
    for (size_t i = 0; i < n; i += 2) odd.push_back(h[i]);
    center = h[2 * k + 1];
    return k;
}

// Keeps the last `history` pairs of `v` at its front.
void keepHistory(std::vector<float>& v, size_t history) {
    if (v.size() > 2 * history) std::copy(v.end() - std::ptrdiff_t(2 * history), v.end(), v.begin());
    v.resize(2 * history);
}

}  // namespace

void HalfBandPairDecimator::configure(double passHz, double inRate, double attenuationDb) {
    std::vector<float> odd;
    k_ = designHalfBand(std::max(inRate / 2.0 - 2.0 * passHz, inRate * 0.01), inRate, attenuationDb, odd, center_);
    taps2_ = detail::duplicateTaps(odd.data(), odd.size());
    reset();
}

void HalfBandPairDecimator::reset() {
    even_.assign(2 * (k_ + 1), 0.0f);
    odd_.assign(2 * (2 * k_ + 1), 0.0f);
    parity_ = 0;
}

size_t HalfBandPairDecimator::process(const float* in, size_t count, float* out) {
    const size_t histEven = k_ + 1, histOdd = 2 * k_ + 1;
    // parity_ = 1: in[0] is an odd sample.
    const size_t evens = (count + 1 - parity_) / 2, odds = (count + parity_) / 2;
    even_.resize(2 * (histEven + evens));
    odd_.resize(2 * (histOdd + odds));
    float* e = &even_[2 * histEven];
    float* o = &odd_[2 * histOdd];
    for (size_t i = parity_, j = 0; i < count; i += 2, j++) {
        e[2 * j] = in[2 * i];
        e[2 * j + 1] = in[2 * i + 1];
    }
    for (size_t i = 1 - parity_, j = 0; i < count; i += 2, j++) {
        o[2 * j] = in[2 * i];
        o[2 * j + 1] = in[2 * i + 1];
    }
    // Output j: the 2k+2 odd samples ending at odd sample j, plus the centre:
    // the even sample k places before the one that precedes odd sample j.
    const size_t taps = 2 * k_ + 2;
    for (size_t j = 0; j < odds; j++) {
        const float* c = &even_[2 * (histEven + j - parity_ - k_)];
        float l[4];
        laneDot(&odd_[2 * j], taps2_.data(), 2 * taps, l);
        out[2 * j] = (l[0] + l[2]) + center_ * c[0];
        out[2 * j + 1] = (l[1] + l[3]) + center_ * c[1];
    }
    keepHistory(even_, histEven);
    keepHistory(odd_, histOdd);
    parity_ = (parity_ + count) & 1;
    return odds;
}

void HalfBandPairInterpolator::configure(double passHz, double outRate, double attenuationDb) {
    std::vector<float> odd;
    k_ = designHalfBand(std::max(outRate / 2.0 - 2.0 * passHz, outRate * 0.01), outRate, attenuationDb, odd, center_);
    for (float& t : odd) t *= 2.0f;  // zero stuffing halves the level
    center_ *= 2.0f;
    taps2_ = detail::duplicateTaps(odd.data(), odd.size());
    reset();
}

void HalfBandPairInterpolator::reset() { buffer_.assign(2 * (2 * k_ + 1), 0.0f); }

void HalfBandPairInterpolator::process(const float* in, size_t count, float* out) {
    const size_t hist = 2 * k_ + 1, taps = 2 * k_ + 2;
    buffer_.resize(2 * (hist + count));
    std::copy(in, in + 2 * count, buffer_.begin() + std::ptrdiff_t(2 * hist));
    for (size_t m = 0; m < count; m++) {
        // Between input samples: the odd taps over the last 2k+2 inputs;
        // on an input sample: that sample, delayed by k.
        float l[4];
        laneDot(&buffer_[2 * m], taps2_.data(), 2 * taps, l);
        out[4 * m] = l[0] + l[2];
        out[4 * m + 1] = l[1] + l[3];
        out[4 * m + 2] = center_ * buffer_[2 * (m + k_ + 1)];
        out[4 * m + 3] = center_ * buffer_[2 * (m + k_ + 1) + 1];
    }
    keepHistory(buffer_, hist);
}

// ---- IqBalancer -----------------------------------------------------------------

void IqBalancer::reset() {
    re_ = 0.0f;
    im_ = 0.0f;
}

void IqBalancer::update(const Complex* bins, size_t n) {
    if (n < 16) return;
    power_.resize(n);
    for (size_t k = 0; k < n; k++) power_[k] = magSquared(bins[k]);
    scratch_ = power_;
    std::nth_element(scratch_.begin(), scratch_.begin() + std::ptrdiff_t(n / 2), scratch_.end());
    const float floor = 10.0f * scratch_[n / 2];  // 10 dB above the median (the noise floor)

    // Bins with a signal at least 15 dB stronger than its mirror: there the
    // mirror is (mostly) the image, Z(f) Z(-f) / |Z(strong)|^2 = b.
    Complex num(0.0f, 0.0f);
    double den = 0.0;
    for (size_t k = 1; k < n / 2; k++) {
        const float a = power_[k], m = power_[n - k];
        const float strong = std::max(a, m), weak = std::min(a, m);
        if (strong < floor || strong < 31.6f * weak) continue;
        num += cmul(bins[k], bins[n - k]);
        den += strong;
    }
    if (den <= 0.0) return;

    // Measured on the corrected signal: integrate the residual.
    Complex b = correction() + num * float(0.05 / den);
    const float mag = std::abs(b);
    if (mag > 0.3f) b *= 0.3f / mag;  // worse than 10 dB image rejection is not an I/Q error
    re_ = b.real();
    im_ = b.imag();
}

void IqBalancer::process(Complex* data, size_t count) const {
    const Complex b = correction();
    if (b == Complex(0.0f, 0.0f)) return;
    for (size_t k = 0; k < count; k++) data[k] -= cmul(b, std::conj(data[k]));
}

}  // namespace SharkSDR::dsp
