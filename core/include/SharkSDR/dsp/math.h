#pragma once

#include <SharkSDR/types.h>

#include <cmath>

namespace SharkSDR::dsp {

// atan2 approximation, max error ~0.0015 rad. Much faster than std::atan2.
inline float fastAtan2(float y, float x) {
    const float ay = std::fabs(y) + 1e-20f;
    float angle;
    if (x >= 0.0f) {
        float r = (x - ay) / (x + ay);
        angle = 0.1963f * r * r * r - 0.9817f * r + float(kPi / 4.0);
    } else {
        float r = (x + ay) / (ay - x);
        angle = 0.1963f * r * r * r - 0.9817f * r + float(3.0 * kPi / 4.0);
    }
    return y < 0.0f ? -angle : angle;
}

// atan2 accurate to ~1e-7 rad (float rounding), several times faster than
// std::atan2: octant reduction and the polynomial of Abramowitz & Stegun 4.4.49.
inline float accurateAtan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float hi = ax > ay ? ax : ay, lo = ax > ay ? ay : ax;
    const float t = lo / (hi + 1e-30f);  // [0, 1]
    const float s = t * t;
    float r = 0.0028662257f;
    r = r * s - 0.0161657367f;
    r = r * s + 0.0429096138f;
    r = r * s - 0.0752896400f;
    r = r * s + 0.1065626393f;
    r = r * s - 0.1420889944f;
    r = r * s + 0.1999355085f;
    r = r * s - 0.3333314528f;
    r = t + t * s * r;
    if (ay > ax) r = float(kPi / 2.0) - r;
    if (x < 0.0f) r = float(kPi) - r;
    return y < 0.0f ? -r : r;
}

inline float powerToDb(float p) { return 10.0f * std::log10(p + 1e-20f); }
inline float amplitudeToDb(float a) { return 20.0f * std::log10(a + 1e-20f); }
inline float dbToAmplitude(float db) { return std::pow(10.0f, db / 20.0f); }

}  // namespace SharkSDR::dsp
