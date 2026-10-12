#pragma once

#include <SharkSDR/demodulator.h>
#include <SharkSDR/dsp/filter.h>
#include <SharkSDR/dsp/nco.h>
#include <SharkSDR/dsp/noise_reduction.h>
#include <SharkSDR/dsp/resampler.h>
#include <SharkSDR/export.h>
#include <SharkSDR/types.h>

#include <vector>

namespace SharkSDR::dsp {

// A complete single channel receiver:
//
//   wideband IQ -> NCO (shift VFO to 0 Hz) -> power-of-two decimation
//     -> resampling to the demodulator's channel rate -> channel filter
//     -> signal level / squelch -> demodulator -> audio low-pass + resampling
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
    void reset();

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

    bool dirty_ = true;
    bool filterDirty_ = true;

    Nco nco_;
    PowerDecimator decimator_;
    Resampler<Complex> channelResampler_;
    ComplexFirFilter channelFilter_;
    Resampler<Stereo> audioResampler_;

    std::vector<Complex> work_;
    std::vector<Complex> channel_;
    std::vector<Stereo> demodOut_;
};

}  // namespace SharkSDR::dsp
