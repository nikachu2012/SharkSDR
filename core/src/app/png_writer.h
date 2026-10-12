#pragma once

#include <cstdint>
#include <filesystem>

namespace SharkSDR {

// Writes an RGBA8 image as an (uncompressed) PNG file.
bool writePng(const std::filesystem::path& path, int width, int height, const uint8_t* rgba);

}  // namespace SharkSDR
