#pragma once

#include <SharkSDR/export.h>

#include <vector>

namespace SharkSDR::dsp {

enum class WindowType { Rectangular, Hann, Hamming, Blackman, BlackmanHarris, Nuttall };

SHARKSDR_API const char* windowName(WindowType type);
constexpr int kWindowTypeCount = 6;

// Symmetric window of length n (periodic=true gives the DFT-even variant used for spectra).
SHARKSDR_API std::vector<float> makeWindow(WindowType type, size_t n, bool periodic = false);

// Kaiser window, used for FIR design.
SHARKSDR_API std::vector<float> kaiserWindow(size_t n, double beta);

}  // namespace SharkSDR::dsp
