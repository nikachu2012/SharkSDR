#include "app/png_writer.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace SharkSDR {
namespace {

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void put32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void chunk(std::ofstream& out, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buf;
    put32be(buf, uint32_t(data.size()));
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    uint32_t crc = crc32(buf.data() + 4, buf.size() - 4);
    put32be(buf, crc);
    out.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(buf.size()));
}

}  // namespace

bool writePng(const std::filesystem::path& path, int width, int height, const uint8_t* rgba) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    out.write(reinterpret_cast<const char*>(sig), 8);

    std::vector<uint8_t> ihdr;
    put32be(ihdr, uint32_t(width));
    put32be(ihdr, uint32_t(height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8 bit RGBA
    chunk(out, "IHDR", ihdr);

    // Raw scanlines (filter byte 0), wrapped in stored deflate blocks.
    const size_t rowBytes = size_t(width) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((rowBytes + 1) * size_t(height));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + size_t(y) * rowBytes, rgba + size_t(y + 1) * rowBytes);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n & 0xff));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n & 0xff));
        z.push_back(uint8_t((~n >> 8) & 0xff));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(pos), raw.begin() + std::ptrdiff_t(pos + n));
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put32be(z, (b << 16) | a);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return bool(out);
}

}  // namespace SharkSDR
