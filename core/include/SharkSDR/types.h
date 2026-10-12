#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>

namespace SharkSDR {

// Baseband IQ sample. Full scale is a magnitude of 1.0.
using Complex = std::complex<float>;

// One stereo audio frame. Full scale is +-1.0.
struct Stereo {
    float l = 0.0f;
    float r = 0.0f;

    Stereo() = default;
    Stereo(float left, float right) : l(left), r(right) {}
    explicit Stereo(float mono) : l(mono), r(mono) {}

    Stereo& operator+=(const Stereo& o) { l += o.l; r += o.r; return *this; }
    friend Stereo operator+(Stereo a, const Stereo& b) { return a += b; }
    friend Stereo operator*(Stereo a, float g) { return Stereo(a.l * g, a.r * g); }
    friend Stereo operator*(float g, Stereo a) { return Stereo(a.l * g, a.r * g); }
};

// Complex multiplication without the NaN/Inf handling of std::complex
// (which is slow with some compilers).
inline Complex cmul(const Complex& a, const Complex& b) {
    return {a.real() * b.real() - a.imag() * b.imag(),
            a.real() * b.imag() + a.imag() * b.real()};
}

// a * conj(b)
inline Complex cmulConj(const Complex& a, const Complex& b) {
    return {a.real() * b.real() + a.imag() * b.imag(),
            a.imag() * b.real() - a.real() * b.imag()};
}

inline float magSquared(const Complex& c) { return c.real() * c.real() + c.imag() * c.imag(); }

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

}  // namespace SharkSDR
