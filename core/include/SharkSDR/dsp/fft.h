#pragma once

#include <SharkSDR/export.h>
#include <SharkSDR/types.h>

#include <vector>

namespace SharkSDR::dsp {

// In-place radix-2 complex FFT (forward transform, no scaling).
class SHARKSDR_API Fft {
public:
    Fft() = default;
    explicit Fft(size_t size) { resize(size); }

    // `size` must be a power of two.
    void resize(size_t size);
    size_t size() const { return size_; }

    void forward(Complex* data) const;

private:
    size_t size_ = 0;
    std::vector<uint32_t> bitReverse_;
    std::vector<Complex> twiddles_;
};

}  // namespace SharkSDR::dsp
