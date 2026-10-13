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

// Spectral noise reduction for audio: the hiss of weak FM stations, band
// noise on SSB / AM.
//
// Short-time FFT with Hann windows and 75 % overlap. The noise spectrum is
// the minimum of the smoothed power of each bin over the last ~1.5 s
// (minimum statistics), so it follows changing noise without needing pauses
// in the programme. Each bin gets a log-spectral amplitude (MMSE-LSA) gain
// from the decision-directed a-priori SNR, which keeps "musical noise" low.
// Both channels get the same gain, so the stereo image does not move.
//
// Adds a constant delay of about 21 ms.
class SHARKSDR_API AudioNoiseReduction {
public:
    // reductionDb: how far noise-only bins are attenuated (the gain floor).
    void configure(double sampleRate, float reductionDb);
    void setReduction(float reductionDb);
    void reset();
    void process(Stereo* data, size_t count);  // in place

private:
    void processFrame();

    size_t n_ = 0, hop_ = 0, bins_ = 0;
    Fft fft_;
    std::vector<float> window_;
    std::vector<Complex> frame_;  // last n_ inputs as L + jR, oldest first
    std::vector<Complex> work_;
    std::vector<Complex> sum_;    // overlap-add accumulator
    std::vector<Complex> ready_;  // finished output samples
    size_t fill_ = 0;

    // Per bin.
    std::vector<float> smooth_;    // smoothed power
    std::vector<float> subMin_;    // minimum in the current sub-window
    std::vector<float> minima_;    // minima of the previous sub-windows
    std::vector<float> cleanSnr_;  // previous frame's estimated clean power / noise
    std::vector<float> noise_;
    std::vector<float> gain_;
    std::vector<float> scratch_;
    size_t subLength_ = 36;  // frames per sub-window
    size_t frameInSub_ = 0, subIndex_ = 0, frames_ = 0;
    float floor_ = 0.25f;
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
