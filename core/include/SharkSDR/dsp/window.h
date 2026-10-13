#pragma once

#include <SharkSDR/export.h>

#include <vector>

namespace SharkSDR::dsp {

// BlackmanHarris is the 4-term window (side lobes -92 dB), BlackmanHarris7
// the 7-term one of SDR# (-180 dB) for receivers with more dynamic range.
enum class WindowType { Rectangular, Hann, Hamming, Blackman, BlackmanHarris, Nuttall, BlackmanHarris7 };

SHARKSDR_API const char* windowName(WindowType type);
constexpr int kWindowTypeCount = 7;

// Symmetric window of length n (periodic=true gives the DFT-even variant used for spectra).
SHARKSDR_API std::vector<float> makeWindow(WindowType type, size_t n, bool periodic = false);

// Kaiser window, used for FIR design.
SHARKSDR_API std::vector<float> kaiserWindow(size_t n, double beta);

}  // namespace SharkSDR::dsp
