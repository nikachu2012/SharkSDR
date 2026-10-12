#pragma once

#include <algorithm>
#include <cmath>

namespace SharkSDR::dsp {

// Envelope following automatic gain control: fast attack, slow decay.
class Agc {
public:
    // attack/decay are time constants in seconds.
    void configure(double sampleRate, double attack = 0.002, double decay = 0.4,
                   float target = 0.3f, float maxGainDb = 90.0f) {
        attack_ = float(1.0 - std::exp(-1.0 / (attack * sampleRate)));
        decay_ = float(1.0 - std::exp(-1.0 / (decay * sampleRate)));
        target_ = target;
        maxGain_ = std::pow(10.0f, maxGainDb / 20.0f);
    }
    void reset() { level_ = 0.0f; }

    // Feed the magnitude of the current sample; returns the gain to apply.
    float gain(float magnitude) {
        if (magnitude > level_) level_ += attack_ * (magnitude - level_);
        else level_ += decay_ * (magnitude - level_);
        return std::min(target_ / std::max(level_, 1e-9f), maxGain_);
    }

private:
    float attack_ = 0.01f, decay_ = 0.0001f;
    float target_ = 0.3f;
    float maxGain_ = 1e4f;
    float level_ = 0.0f;
};

// Limiter keeping audio within +-1: linear up to 0.8, then a smooth knee.
inline float softClip(float x) {
    float a = std::fabs(x);
    if (a <= 0.8f) return x;
    float y = 0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f);
    return x < 0.0f ? -y : y;
}

}  // namespace SharkSDR::dsp
