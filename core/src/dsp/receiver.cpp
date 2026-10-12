#include <SharkSDR/dsp/math.h>
#include <SharkSDR/dsp/receiver.h>

#include <algorithm>
#include <cmath>

namespace SharkSDR::dsp {

Receiver::Receiver() = default;
Receiver::~Receiver() = default;

void Receiver::setInputRate(double sampleRate) {
    if (sampleRate == inputRate_) return;
    inputRate_ = sampleRate;
    dirty_ = true;
}

void Receiver::setOffset(double hz) { offset_ = hz; }

void Receiver::setDemodulator(Demodulator* demod) {
    if (demod == demod_) return;
    demod_ = demod;
    dirty_ = true;
}

void Receiver::setBandwidth(double hz) {
    if (hz == bandwidth_) return;
    bandwidth_ = hz;
    filterDirty_ = true;
    if (demod_) demod_->setBandwidth(hz);
}

void Receiver::setAudioRate(double sampleRate) {
    if (sampleRate == audioRate_) return;
    audioRate_ = sampleRate;
    dirty_ = true;
    audioFilterDirty_ = true;
}

void Receiver::setSquelch(bool enabled, float levelDb) {
    squelchEnabled_ = enabled;
    squelchDb_ = levelDb;
}

void Receiver::setAudioFilters(bool highpass, double highpassHz, bool lowpass, double lowpassHz) {
    if (highpass == highpassEnabled_ && lowpass == lowpassEnabled_ && highpassHz == highpassHz_ && lowpassHz == lowpassHz_)
        return;
    highpassEnabled_ = highpass;
    lowpassEnabled_ = lowpass;
    highpassHz_ = highpassHz;
    lowpassHz_ = lowpassHz;
    audioFilterDirty_ = true;
}

void Receiver::reset() {
    highpass_.reset();
    lowpass_.reset();
    nco_.reset();
    decimator_.reset();
    channelResampler_.reset();
    channelFilter_.reset();
    audioResampler_.reset();
    if (demod_) demod_->reset();
}

void Receiver::rebuild() {
    dirty_ = false;
    filterDirty_ = true;
    if (!demod_ || inputRate_ <= 0.0) return;

    channelRate_ = demod_->channelRate();

    // 1) cheap power-of-two decimation down to 2..4x the channel rate
    //    (2) the fractional resampler depends on the bandwidth, see rebuildChannelFilter())
    decimator_.configure(inputRate_, 2.0 * channelRate_, 0.5 * channelRate_);

    // 4) audio low-pass + resampling to the sound card rate
    const double nyq = 0.5 * std::min(channelRate_, audioRate_);
    double cutoff = std::min(demod_->audioCutoff(), nyq);
    double transition = std::clamp(cutoff * 0.2, 200.0, 4000.0);
    if (cutoff < nyq && cutoff + transition * 0.5 > nyq) cutoff = nyq - transition * 0.5;
    audioResampler_.configure(channelRate_, audioRate_, cutoff, transition);

    demod_->setBandwidth(bandwidth_);
    demod_->reset();
}

void Receiver::rebuildChannelFilter() {
    filterDirty_ = false;
    if (!demod_ || channelRate_ <= 0.0) return;

    const double bw = std::clamp(bandwidth_, demod_->minBandwidth(), std::min(demod_->maxBandwidth(), channelRate_));
    double lo = -bw * 0.5, hi = bw * 0.5;
    if (demod_->bandShape() == BandShape::Upper) lo = 0.0, hi = bw;
    if (demod_->bandShape() == BandShape::Lower) lo = -bw, hi = 0.0;
    const double edge = std::max(std::fabs(lo), std::fabs(hi));
    const double maxTransition = std::max(10.0, 0.5 * channelRate_ - edge);
    const double transition = std::min(std::max(bw * 0.1, 50.0), maxTransition);

    // 2) fractional resampling to the exact channel rate. Only the channel
    //    [-edge, edge] (plus the channel filter skirt) must stay alias free,
    //    so narrow channels get a wide transition band and a short filter.
    const double mid = decimator_.outputRate();
    const double m = std::min(mid, channelRate_);
    const double pass = std::min(edge + transition * 0.5, 0.45 * m);
    const double stop = std::max(m - pass, pass + 0.1 * m);
    channelResampler_.configure(mid, channelRate_, 0.5 * (pass + stop), stop - pass);

    // 3) channel filter matching the VFO bandwidth and band shape
    if (bw >= channelRate_ * 0.99) {
        channelFilter_.setTaps({});
        return;
    }
    channelFilter_.setTaps(designComplexBandpass(lo, hi, transition, channelRate_, 60.0));
}

void Receiver::process(const Complex* in, size_t count, std::vector<Stereo>& audio) {
    if (dirty_) rebuild();
    if (filterDirty_) rebuildChannelFilter();
    if (!demod_ || inputRate_ <= 0.0 || count == 0) return;

    work_.resize(count);
    nco_.setFrequency(-offset_, inputRate_);
    nco_.mix(in, count, work_.data());
    const size_t n = decimator_.process(work_.data(), count, work_.data());

    channel_.resize(channelResampler_.maxOutput(n));
    const size_t m = channelResampler_.process(work_.data(), n, channel_.data());
    if (m == 0) return;
    channelFilter_.process(channel_.data(), m, channel_.data());

    // Signal level (50 ms time constant) and squelch with 1.5 dB hysteresis.
    double power = 0.0;
    for (size_t i = 0; i < m; i++) power += magSquared(channel_[i]);
    const float db = powerToDb(float(power / double(m)));
    const float alpha = float(1.0 - std::exp(-double(m) / (channelRate_ * 0.05)));
    levelDb_ = levelDb_ <= -149.0f ? db : levelDb_ + alpha * (db - levelDb_);
    if (!squelchEnabled_) squelchOpen_ = true;
    else if (squelchOpen_) squelchOpen_ = levelDb_ >= squelchDb_ - 1.5f;
    else squelchOpen_ = levelDb_ >= squelchDb_;

    demodOut_.resize(m);
    demod_->process(channel_.data(), m, demodOut_.data());
    if (!squelchOpen_) std::fill(demodOut_.begin(), demodOut_.end(), Stereo());

    const size_t old = audio.size();
    audio.resize(old + audioResampler_.maxOutput(m));
    const size_t produced = audioResampler_.process(demodOut_.data(), m, audio.data() + old);
    audio.resize(old + produced);

    if (audioFilterDirty_) {
        audioFilterDirty_ = false;
        if (highpassEnabled_) highpass_.configure(true, highpassHz_, audioRate_);
        if (lowpassEnabled_) lowpass_.configure(false, lowpassHz_, audioRate_);
    }
    if (highpassEnabled_) highpass_.process(audio.data() + old, produced);
    if (lowpassEnabled_) lowpass_.process(audio.data() + old, produced);
}

}  // namespace SharkSDR::dsp
