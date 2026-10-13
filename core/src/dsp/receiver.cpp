#include <SharkSDR/dsp/math.h>
#include <SharkSDR/dsp/receiver.h>

#include <algorithm>
#include <cmath>

namespace SharkSDR::dsp {

namespace {
// Stop-band attenuation of each stage. The half-band decimators and the
// resampler see the strongest signals of the whole band (other stations), so
// they get the most; an 8-bit RTL-SDR covers ~50 dB, but its processing gain
// and stronger receivers need the rest.
constexpr double kDecimatorDb = 100.0;
constexpr double kResamplerDb = 90.0;
constexpr double kChannelDb = 90.0;
constexpr double kAudioDb = 80.0;
}  // namespace

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
    if (demod_) demod_->setMpxHook(nullptr);
    demod_ = demod;
    if (demod_) demod_->setMpxHook(hooks_.mpx);
    dirty_ = true;
}

void Receiver::setStreamHooks(StreamHooks hooks) {
    hooks_ = std::move(hooks);
    if (demod_) demod_->setMpxHook(hooks_.mpx);
}

void Receiver::setBypassDemodulation(bool bypass) {
    if (bypass == bypass_) return;
    bypass_ = bypass;
    if (!bypass && demod_) demod_->reset();  // its state is stale by now
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

void Receiver::setNoiseReduction(bool enabled, float reductionDb) {
    if (enabled == nrEnabled_ && reductionDb == nrDb_) return;
    if (enabled && !nrEnabled_) nrRate_ = 0.0;  // start from a clean state
    nrEnabled_ = enabled;
    nrDb_ = reductionDb;
    audioFilterDirty_ = true;
}

void Receiver::reset() {
    highpass_.reset();
    lowpass_.reset();
    nco_.reset();
    decimator_.reset();
    channelResampler_.reset();
    channelFilter_.reset();
    complexChannelFilter_.reset();
    audioDecimator_.reset();
    audioResampler_.reset();
    if (demod_) demod_->reset();
}

void Receiver::rebuild() {
    dirty_ = false;
    filterDirty_ = true;
    audioCutoff_ = -1.0;
    if (!demod_ || inputRate_ <= 0.0) return;

    channelRate_ = demod_->channelRate();

    // 1) half-band decimation by two down to 2..4x the channel rate
    //    (2) the fractional resampler depends on the bandwidth, see rebuildChannelFilter())
    decimator_.configure(inputRate_, 2.0 * channelRate_, 0.5 * channelRate_, kDecimatorDb);

    demod_->setBandwidth(bandwidth_);
    demod_->reset();
}

void Receiver::rebuildAudio() {
    audioCutoff_ = demod_->audioCutoff();

    // 4) audio low-pass + resampling to the sound card rate
    const double nyq = 0.5 * std::min(channelRate_, audioRate_);
    double cutoff = std::min(audioCutoff_, nyq);
    double transition = std::clamp(cutoff * 0.2, 200.0, 4000.0);
    if (cutoff < nyq && cutoff + transition * 0.5 > nyq) cutoff = nyq - transition * 0.5;

    // First halve the rate while it stays well above the cut-off (cheap
    // half-band stages), so that the sharp low-pass runs at a low rate.
    const double stop = cutoff + transition * 0.5;
    audioDecimator_.configure(channelRate_, 2.5 * stop, stop, kAudioDb);
    audioResampler_.configure(audioDecimator_.outputRate(), audioRate_, cutoff, transition, kAudioDb);
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
    channelResampler_.configure(mid, channelRate_, 0.5 * (pass + stop), stop - pass, kResamplerDb);

    // 3) channel filter matching the VFO bandwidth and band shape (real taps
    //    for pass bands centred on the VFO: half the work of complex ones)
    complexChannel_ = lo != -hi;
    if (bw >= channelRate_ * 0.99) {
        channelFilter_.setTaps({});
        complexChannel_ = false;
    } else if (complexChannel_) {
        complexChannelFilter_.setTaps(designComplexBandpass(lo, hi, transition, channelRate_, kChannelDb));
    } else {
        channelFilter_.setTaps(designLowpass(hi, transition, channelRate_, kChannelDb));
    }
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
    if (complexChannel_) complexChannelFilter_.process(channel_.data(), m, channel_.data());
    else channelFilter_.process(channel_.data(), m, channel_.data());
    if (hooks_.channel) hooks_.channel(channel_.data(), m, channelRate_);

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
    if (bypass_) std::fill(demodOut_.begin(), demodOut_.end(), Stereo());
    else demod_->process(channel_.data(), m, demodOut_.data());
    if (!squelchOpen_) std::fill(demodOut_.begin(), demodOut_.end(), Stereo());
    if (hooks_.demodulated) hooks_.demodulated(demodOut_.data(), m, channelRate_);

    // The cut-off follows the bandwidth (and e.g. the CW tone).
    if (demod_->audioCutoff() != audioCutoff_) rebuildAudio();
    const size_t d = audioDecimator_.process(demodOut_.data(), m, demodOut_.data());
    const size_t old = audio.size();
    audio.resize(old + audioResampler_.maxOutput(d));
    const size_t produced = audioResampler_.process(demodOut_.data(), d, audio.data() + old);
    audio.resize(old + produced);

    if (audioFilterDirty_) {
        audioFilterDirty_ = false;
        if (highpassEnabled_) highpass_.configure(true, highpassHz_, audioRate_);
        if (lowpassEnabled_) lowpass_.configure(false, lowpassHz_, audioRate_);
        if (nrEnabled_ && nrRate_ != audioRate_) {
            nr_.configure(audioRate_, nrDb_);
            nrRate_ = audioRate_;
        }
        nr_.setReduction(nrDb_);
    }
    if (nrEnabled_) nr_.process(audio.data() + old, produced);
    if (highpassEnabled_) highpass_.process(audio.data() + old, produced);
    if (lowpassEnabled_) lowpass_.process(audio.data() + old, produced);
    if (hooks_.audio && produced > 0) hooks_.audio(audio.data() + old, produced, audioRate_);
}

}  // namespace SharkSDR::dsp
