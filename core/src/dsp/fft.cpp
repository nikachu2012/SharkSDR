#include <SharkSDR/dsp/fft.h>

#include <cmath>
#include <utility>

namespace SharkSDR::dsp {

void Fft::resize(size_t size) {
    if (size == size_) return;
    size_ = size;
    int bits = 0;
    while ((size_t(1) << bits) < size) bits++;

    bitReverse_.resize(size);
    for (size_t i = 0; i < size; i++) {
        uint32_t r = 0;
        for (int b = 0; b < bits; b++)
            if (i & (size_t(1) << b)) r |= uint32_t(1) << (bits - 1 - b);
        bitReverse_[i] = r;
    }

    twiddles_.resize(size / 2);
    for (size_t k = 0; k < size / 2; k++) {
        double a = -kTwoPi * double(k) / double(size);
        twiddles_[k] = Complex(float(std::cos(a)), float(std::sin(a)));
    }
}

void Fft::forward(Complex* a) const {
    const size_t n = size_;
    if (n < 2) return;
    for (size_t i = 0; i < n; i++) {
        size_t j = bitReverse_[i];
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t half = len >> 1;
        const size_t step = n / len;
        for (size_t i = 0; i < n; i += len) {
            Complex* lo = a + i;
            Complex* hi = a + i + half;
            for (size_t j = 0; j < half; j++) {
                Complex v = cmul(hi[j], twiddles_[j * step]);
                Complex u = lo[j];
                lo[j] = u + v;
                hi[j] = u - v;
            }
        }
    }
}

}  // namespace SharkSDR::dsp
