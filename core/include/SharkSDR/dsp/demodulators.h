#pragma once

#include <SharkSDR/demodulator.h>
#include <SharkSDR/export.h>

#include <memory>
#include <string>
#include <vector>

namespace SharkSDR::dsp {

// Built-in modes: WFM, NFM, AM, USB, LSB, DSB, CW, RAW.
SHARKSDR_API std::vector<std::string> builtinDemodulatorNames();

// Returns nullptr for unknown names.
SHARKSDR_API std::unique_ptr<Demodulator> createBuiltinDemodulator(const std::string& name);

}  // namespace SharkSDR::dsp
