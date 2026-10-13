#pragma once

#include <SharkSDR/demodulator.h>
#include <SharkSDR/dsp/filter.h>
#include <SharkSDR/dsp/nco.h>
#include <SharkSDR/dsp/noise_reduction.h>
#include <SharkSDR/dsp/resampler.h>
#include <SharkSDR/export.h>
#include <SharkSDR/types.h>

#include <functional>
#include <vector>

namespace SharkSDR::dsp {

// A complete single channel receiver:
//
//   wideband IQ -> NCO (shift VFO to 0 Hz) -> half-band decimation by 2^n
//     -> resampling to the demodulator's channel rate -> channel filter
//     -> [channel hook] -> signal level / squelch -> demodulator ([mpx hook])
//     -> [demodulated hook] -> half-band decimation of the audio -> audio
//     low-pass + resampling -> noise reduction, filters -> [audio hook]
//
// The application uses one Receiver for the main VFO. Plugins can create
// their own instances (e.g. for multi-channel decoders) and feed them with
// an IQ tap.
//
// Not thread-safe: configure and process from the same thread, or lock.
class SHARKSDR_API Receiver {
public:
    Receiver();
    ~Receiver();

    void setInputRate(double sampleRate);
    void setOffset(double hz);                // VFO frequency relative to the IQ center
    void setDemodulator(Demodulator* demod);  // not owned; nullptr disables audio
    void setBandwidth(double hz);
    void setAudioRate(double sampleRate);
    void setSquelch(bool enabled, float levelDb);
    // Optional audio high-pass / low-pass (8th order Butterworth) after demodulation.
    void setAudioFilters(bool highpass, double highpassHz, bool lowpass, double lowpassHz);
    // Optional spectral noise reduction of the audio (see AudioNoiseReduction).
    void setNoiseReduction(bool enabled, float reductionDb);
    void reset();

    // Optional hooks into the chain, called from process() with the samples
    // in place (they may change them). The application connects the plugins'
    // stream hooks here, see Host::registerStreamHook().
    struct StreamHooks {
        std::function<void(Complex* samples, size_t count, double sampleRate)> channel;  // after the channel filter
        Demodulator::MpxHook mpx;                                                       // WFM composite signal
        std::function<void(Stereo* frames, size_t count, double sampleRate)> demodulated;  // after the squelch
        std::function<void(Stereo* frames, size_t count, double sampleRate)> audio;  // audio rate, after the filters
    };
    void setStreamHooks(StreamHooks hooks);

    // Leaves the demodulator out: the audio stages run on silence (for
    // decoders that produce all the audio themselves).
    void setBypassDemodulation(bool bypass);
    bool bypassDemodulation() const { return bypass_; }

    double inputRate() const { return inputRate_; }
    double offset() const { return offset_; }
    double bandwidth() const { return bandwidth_; }
    double channelRate() const { return channelRate_; }
    Demodulator* demodulator() const { return demod_; }

    // Process `count` wideband samples and append the resulting audio frames
    // (at the audio rate) to `audio`.
    void process(const Complex* in, size_t count, std::vector<Stereo>& audio);

    float signalLevel() const { return levelDb_; }  // channel power, dBFS
    bool squelchOpen() const { return squelchOpen_; }

private:
    void rebuild();
    void rebuildChannelFilter();
    void rebuildAudio();

    double inputRate_ = 0.0;
    double offset_ = 0.0;
    double bandwidth_ = 10000.0;
    double audioRate_ = 48000.0;
    double channelRate_ = 0.0;
    Demodulator* demod_ = nullptr;

    bool squelchEnabled_ = false;
    float squelchDb_ = -60.0f;
    bool squelchOpen_ = true;
    float levelDb_ = -150.0f;

    bool highpassEnabled_ = false, lowpassEnabled_ = false;
    double highpassHz_ = 300.0, lowpassHz_ = 3000.0;
    bool audioFilterDirty_ = true;
    ButterworthFilter highpass_, lowpass_;
    bool nrEnabled_ = false;
    float nrDb_ = 12.0f;
    double nrRate_ = 0.0;  // rate the noise reduction was configured for (0 = needs a reset)
    AudioNoiseReduction nr_;

    bool dirty_ = true;
    bool filterDirty_ = true;
    bool bypass_ = false;
    StreamHooks hooks_;

    Nco nco_;
    PowerDecimator decimator_;
    Resampler<Complex> channelResampler_;
    FirFilter<Complex> channelFilter_;       // symmetric pass bands (real taps)
    ComplexFirFilter complexChannelFilter_;  // USB / LSB
    bool complexChannel_ = false;
    DecimatorCascade<Stereo> audioDecimator_;
    Resampler<Stereo> audioResampler_;
    double audioCutoff_ = -1.0;  // demodulator cut-off the audio stage was built for

    std::vector<Complex> work_;
    std::vector<Complex> channel_;
    std::vector<Stereo> demodOut_;
};

}  // namespace SharkSDR::dsp
