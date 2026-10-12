#pragma once

#include <SharkSDR/dsp/fft.h>
#include <SharkSDR/export.h>
#include <SharkSDR/types.h>

#include <vector>

namespace SharkSDR::dsp {

// IF noise reduction for FM signals.
//
// Over a short time window an FM signal is essentially a single tone at its
// instantaneous frequency, while noise is spread over the whole channel. The
// IF is analysed with a short-time FFT (sqrt-Hann windows, 50 % overlap), only
// the bins around the strongest one are kept, and the signal is resynthesised.
// The result is a band-pass filter that follows the carrier, which removes
// most of the noise that is not close to the instantaneous frequency.
//
// Adds a constant delay of `fftSize` samples.
class SHARKSDR_API FmIfNoiseReduction {
public:
    // fftSize: power of two (e.g. 32..256); keepBins: bins kept on each side of the peak.
    void configure(size_t fftSize, size_t keepBins);
    void reset();
    void process(const Complex* in, size_t count, Complex* out);  // in-place allowed

private:
    void processFrame();

    size_t n_ = 0, hop_ = 0, keep_ = 0;
    Fft fft_;
    std::vector<float> window_;
    std::vector<Complex> frame_;    // last n_ input samples, oldest first
    std::vector<Complex> work_;
    std::vector<Complex> tail_;     // second half of the previous frame (overlap-add)
    std::vector<Complex> ready_;    // finished output samples
    std::vector<float> power_;
    size_t fill_ = 0;
};

// Biquad section (RBJ audio EQ cookbook), real-valued, used for audio filters.
class Biquad {
public:
    void setLowpass(double cutoffHz, double q, double sampleRate) { design(cutoffHz, q, sampleRate, false); }
    void setHighpass(double cutoffHz, double q, double sampleRate) { design(cutoffHz, q, sampleRate, true); }
    void reset() { z1_ = z2_ = 0.0; }
    float process(float x) {
        const double y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return float(y);
    }

private:
    SHARKSDR_API void design(double cutoffHz, double q, double sampleRate, bool highpass);
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double z1_ = 0, z2_ = 0;
};

// Even order Butterworth low/high-pass made of cascaded biquads, applied to
// both channels of a stereo stream.
class SHARKSDR_API ButterworthFilter {
public:
    void configure(bool highpass, double cutoffHz, double sampleRate, int order = 8);
    void reset();
    void process(Stereo* data, size_t count);

private:
    std::vector<Biquad> left_, right_;
};

}  // namespace SharkSDR::dsp
