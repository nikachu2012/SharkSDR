#pragma once

#include <SharkSDR/export.h>
#include <SharkSDR/types.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace SharkSDR::dsp {

// Automatic gain control for the linear modes (SSB, CW, DSB), modelled on
// the AGC of SDR# (which comes from CuteSDR):
//
// - The signal is delayed by 15 ms while its level is measured without the
//   delay, so the gain is already down when a strong signal arrives: no
//   overshoot (clipping, a click) at the start of every word.
// - The level is the peak magnitude over a sliding 18 ms window, in dB. A
//   fast "attack" average and a slow "decay" average follow it and the
//   higher of the two sets the gain. With `hang` the decay average is held
//   for the decay time after the signal drops, then released quickly.
// - Below the threshold the gain stops rising, so the band noise between
//   transmissions is not pulled up to full volume.
class SHARKSDR_API Agc {
public:
    struct Settings {
        bool enabled = true;
        bool hang = false;
        double decay = 0.4;          // seconds
        float thresholdDb = -90.0f;  // input level (dBFS) below which the gain stays fixed
        float manualGainDb = 50.0f;  // gain while disabled
        float target = 0.5f;         // output peak level
    };

    // Keeps the state unless the sample rate changes, so settings can be
    // changed while a signal is being received.
    void configure(double sampleRate, const Settings& settings);
    void reset();

    // Levels `data` in place; the output lags the input by delay() samples.
    void process(Complex* data, size_t count);
    size_t delay() const { return delayLine_.size(); }
    float gainDb() const { return gainDb_; }

private:
    float slidingPeak(float levelDb);
    float targetDb() const;

    Settings settings_;
    double rate_ = 0.0;
    float attackRise_ = 1.0f, attackFall_ = 1.0f, decayRise_ = 1.0f, decayFall_ = 1.0f;
    uint64_t hangSamples_ = 0;

    std::vector<Complex> delayLine_;
    size_t delayPos_ = 0;
    // Sliding maximum: a queue of the samples that can still become the
    // window's peak (decreasing values), as a ring buffer.
    std::vector<uint64_t> peakIndex_;
    std::vector<float> peakValue_;
    size_t peakHead_ = 0, peakCount_ = 0, window_ = 1;
    uint64_t sample_ = 0;

    float attackDb_ = -200.0f, decayDb_ = -200.0f;
    uint64_t hangCount_ = 0;
    float gainDb_ = 0.0f;
};

// Limiter keeping audio within +-1: linear up to 0.8, then a smooth knee.
inline float softClip(float x) {
    float a = std::fabs(x);
    if (a <= 0.8f) return x;
    float y = 0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f);
    return x < 0.0f ? -y : y;
}

}  // namespace SharkSDR::dsp
