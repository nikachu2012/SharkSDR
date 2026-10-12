#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace SharkSDR {

struct Colormap {
    std::string name;
    std::array<uint32_t, 256> lut;  // RGBA (IM_COL32 layout)
};

const std::vector<Colormap>& colormaps();

}  // namespace SharkSDR
