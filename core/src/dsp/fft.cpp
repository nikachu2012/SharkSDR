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

    // The twiddles of each stage, stored contiguously: the stage that merges
    // blocks of `half` samples uses W_{2 half}^j (j < half), at [half - 1 + j].
    twiddles_.resize(size > 1 ? size - 1 : 0);
    for (size_t half = 1; half < size; half <<= 1) {
        for (size_t j = 0; j < half; j++) {
            const double a = -kPi * double(j) / double(half);
            twiddles_[half - 1 + j] = Complex(float(std::cos(a)), float(std::sin(a)));
        }
    }
}

void Fft::forward(Complex* a) const {
    const size_t n = size_;
    if (n < 2) return;
    for (size_t i = 0; i < n; i++) {
        size_t j = bitReverse_[i];
        if (i < j) std::swap(a[i], a[j]);
    }

    int stages = 0;
    while ((size_t(1) << stages) < n) stages++;
    size_t half = 1;
    if (stages % 2 == 1) {
        // Odd number of stages: one radix-2 stage (no multiplications) first.
        for (size_t i = 0; i < n; i += 2) {
            const Complex u = a[i], v = a[i + 1];
            a[i] = u + v;
            a[i + 1] = u - v;
        }
        half = 2;
    }

    // Two radix-2 stages per pass over the data (radix 2^2): half the memory
    // traffic of one pass per stage, which is what limits large transforms.
    for (; half < n; half <<= 2) {
        const Complex* w1 = &twiddles_[half - 1];      // W_{2 half}^j
        const Complex* w2 = &twiddles_[2 * half - 1];  // W_{4 half}^j
        for (size_t i = 0; i < n; i += 4 * half) {
            Complex* p0 = a + i;
            Complex* p1 = p0 + half;
            Complex* p2 = p1 + half;
            Complex* p3 = p2 + half;
            for (size_t j = 0; j < half; j++) {
                const Complex b = cmul(p1[j], w1[j]);
                const Complex d = cmul(p3[j], w1[j]);
                const Complex a1 = p0[j] + b, b1 = p0[j] - b;
                const Complex c1 = p2[j] + d, d1 = p2[j] - d;
                const Complex c2 = cmul(c1, w2[j]);
                const Complex d2 = cmul(d1, w2[j]);
                const Complex d3(d2.imag(), -d2.real());  // * W_{4 half}^half = -j
                p0[j] = a1 + c2;
                p2[j] = a1 - c2;
                p1[j] = b1 + d3;
                p3[j] = b1 - d3;
            }
        }
    }
}

}  // namespace SharkSDR::dsp
