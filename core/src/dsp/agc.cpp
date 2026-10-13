#include <SharkSDR/dsp/agc.h>

#include <algorithm>
#include <cmath>

namespace SharkSDR::dsp {

namespace {
// Time constants of the SDR# / CuteSDR AGC.
constexpr double kLookahead = 0.015;     // signal delay
constexpr double kWindow = 0.018;        // peak detector window, longer than the delay plus the attack
constexpr double kAttackRise = 0.002;
constexpr double kAttackFall = 0.005;
constexpr double kDecayRiseRatio = 0.3;  // the decay average rises in 0.3 x the decay time
constexpr double kRelease = 0.05;        // decay after the hang time
constexpr float kMinDb = -200.0f;
constexpr float kDbToNeper = 0.11512925f;  // ln(10) / 20
}  // namespace

void Agc::configure(double sampleRate, const Settings& settings) {
    settings_ = settings;
    auto alpha = [&](double tau) { return float(1.0 - std::exp(-1.0 / std::max(tau * sampleRate, 1.0))); };
    attackRise_ = alpha(kAttackRise);
    attackFall_ = alpha(kAttackFall);
    decayRise_ = alpha(settings.decay * kDecayRiseRatio);
    decayFall_ = alpha(settings.hang ? kRelease : settings.decay);
    hangSamples_ = settings.hang ? uint64_t(settings.decay * sampleRate) : 0;
    if (sampleRate == rate_) return;
    rate_ = sampleRate;
    delayLine_.resize(std::max<size_t>(1, size_t(std::lround(kLookahead * sampleRate))));
    window_ = std::max<size_t>(1, size_t(std::lround(kWindow * sampleRate)));
    peakIndex_.resize(window_ + 1);
    peakValue_.resize(window_ + 1);
    reset();
}

void Agc::reset() {
    std::fill(delayLine_.begin(), delayLine_.end(), Complex());
    delayPos_ = 0;
    peakHead_ = peakCount_ = 0;
    sample_ = 0;
    attackDb_ = decayDb_ = kMinDb;
    hangCount_ = 0;
    gainDb_ = settings_.enabled ? targetDb() - settings_.thresholdDb : settings_.manualGainDb;
}

float Agc::targetDb() const { return 20.0f * std::log10(std::max(settings_.target, 1e-6f)); }

float Agc::slidingPeak(float levelDb) {
    const size_t cap = peakIndex_.size();
    // Samples not above the new one can never be the peak again.
    while (peakCount_ > 0 && peakValue_[(peakHead_ + peakCount_ - 1) % cap] <= levelDb) peakCount_--;
    const size_t tail = (peakHead_ + peakCount_) % cap;
    peakIndex_[tail] = sample_;
    peakValue_[tail] = levelDb;
    peakCount_++;
    if (peakIndex_[peakHead_] + window_ <= sample_) {  // left the window
        peakHead_ = (peakHead_ + 1) % cap;
        peakCount_--;
    }
    sample_++;
    return peakValue_[peakHead_];
}

void Agc::process(Complex* data, size_t count) {
    if (delayLine_.empty()) return;
    const float target = targetDb();
    const size_t d = delayLine_.size();
    for (size_t i = 0; i < count; i++) {
        const Complex x = data[i];
        const float peak = slidingPeak(10.0f * std::log10(magSquared(x) + 1e-20f));
        attackDb_ += (peak > attackDb_ ? attackRise_ : attackFall_) * (peak - attackDb_);
        if (peak > decayDb_) {
            decayDb_ += decayRise_ * (peak - decayDb_);
            hangCount_ = 0;
        } else if (hangCount_ < hangSamples_) {
            hangCount_++;  // hold
        } else {
            decayDb_ += decayFall_ * (peak - decayDb_);
        }
        const float level = std::max(std::max(attackDb_, decayDb_), settings_.thresholdDb);
        gainDb_ = settings_.enabled ? target - level : settings_.manualGainDb;

        const Complex delayed = delayLine_[delayPos_];
        delayLine_[delayPos_] = x;
        if (++delayPos_ == d) delayPos_ = 0;
        data[i] = delayed * std::exp(gainDb_ * kDbToNeper);
    }
}

}  // namespace SharkSDR::dsp
