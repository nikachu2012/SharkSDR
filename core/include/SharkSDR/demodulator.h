#pragma once

#include <SharkSDR/types.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

namespace SharkSDR {

// Where the receive passband lies relative to the tuned (VFO) frequency.
enum class BandShape {
    Center,  // [-bw/2, +bw/2]   AM, FM, CW, ...
    Upper,   // [0, +bw]         USB
    Lower,   // [-bw, 0]         LSB
};

// A demodulator turns a channel-filtered IQ stream into audio.
//
// The receiver chain does the heavy lifting before calling process():
//   IQ -> frequency shift -> decimation/resampling to channelRate()
//      -> channel filter (bandwidth/bandShape) -> squelch -> Demodulator
//      -> audio low-pass at audioCutoff() + resampling to the sound card rate
//
// So a demodulator only has to implement the actual demodulation. New modes
// can be added by plugins via Host::registerDemodulator().
//
// Threading: process() runs on the DSP thread; drawSettings(), loadState()
// and saveState() run on the UI thread while the DSP thread is locked out.
class Demodulator {
public:
    virtual ~Demodulator() = default;

    // IQ sample rate the demodulator wants (the audio is produced at the same rate).
    virtual double channelRate() const = 0;

    virtual BandShape bandShape() const { return BandShape::Center; }
    virtual double defaultBandwidth() const = 0;
    virtual double minBandwidth() const { return 100.0; }
    virtual double maxBandwidth() const { return channelRate(); }

    // Default tuning step for this mode.
    virtual double defaultSnap() const { return 1000.0; }

    // Highest audio frequency of interest. The audio is low-pass filtered to
    // this value before resampling (pilot tones, subcarriers etc. are removed).
    virtual double audioCutoff() const { return channelRate() * 0.45; }

    // Called whenever the bandwidth changes.
    virtual void setBandwidth(double hz) {}

    // Reset internal state (called after re-tuning or on mode change).
    virtual void reset() {}

    // Demodulate `count` channel samples into `count` audio frames.
    virtual void process(const Complex* in, size_t count, Stereo* out) = 0;

    // Optional mode specific settings, drawn in the "Radio" panel.
    virtual void drawSettings() {}

    // Optional status line, e.g. "Stereo" for WFM.
    virtual std::string status() const { return {}; }

    // Persistence of the mode specific settings.
    virtual void loadState(const nlohmann::json& state) {}
    virtual void saveState(nlohmann::json& state) const {}
};

using DemodulatorFactory = std::function<std::unique_ptr<Demodulator>()>;

}  // namespace SharkSDR
