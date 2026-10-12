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

inline float powerToDb(float p) { return 10.0f * std::log10(p + 1e-20f); }
inline float amplitudeToDb(float a) { return 20.0f * std::log10(a + 1e-20f); }
inline float dbToAmplitude(float db) { return std::pow(10.0f, db / 20.0f); }

}  // namespace SharkSDR::dsp
