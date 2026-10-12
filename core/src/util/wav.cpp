#include <SharkSDR/util/wav.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace SharkSDR {
namespace {

std::FILE* openFile(const std::filesystem::path& path, bool write) {
#if defined(_WIN32)
    return _wfopen(path.c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

bool seekFile(std::FILE* f, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(f, int64_t(offset), SEEK_SET) == 0;
#else
    return fseeko(f, off_t(offset), SEEK_SET) == 0;
#endif
}

uint16_t bytesPerSample(WavFormat f) {
    switch (f) {
        case WavFormat::U8: return 1;
        case WavFormat::S16: return 2;
        case WavFormat::S24: return 3;
        case WavFormat::F32: return 4;
    }
    return 2;
}

void put16(std::string& s, uint16_t v) {
    s.push_back(char(v & 0xff));
    s.push_back(char(v >> 8));
}
void put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(char((v >> (8 * i)) & 0xff));
}
uint16_t get16(const unsigned char* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t get32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace

// ---- WavWriter ----------------------------------------------------------------

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::filesystem::path& path, uint32_t sampleRate, uint16_t channels, WavFormat format) {
    close();
    if (format == WavFormat::S24) format = WavFormat::S16;  // 24 bit writing not supported
    file_ = openFile(path, true);
    if (!file_) return false;
    std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
    sampleRate_ = sampleRate;
    channels_ = channels;
    format_ = format;
    dataBytes_ = 0;
    writeHeader();
    return true;
}

void WavWriter::writeHeader() {
    const uint16_t bps = bytesPerSample(format_);
    const uint32_t dataSize = uint32_t(std::min<uint64_t>(dataBytes_, 0xFFFFFFFFull - 36));
    std::string h;
    h += "RIFF";
    put32(h, 36 + dataSize);
    h += "WAVEfmt ";
    put32(h, 16);
    put16(h, format_ == WavFormat::F32 ? 3 : 1);
    put16(h, channels_);
    put32(h, sampleRate_);
    put32(h, sampleRate_ * channels_ * bps);
    put16(h, uint16_t(channels_ * bps));
    put16(h, uint16_t(bps * 8));
    h += "data";
    put32(h, dataSize);
    seekFile(file_, 0);
    std::fwrite(h.data(), 1, h.size(), file_);
}

bool WavWriter::write(const float* in, size_t frames) {
    if (!file_) return false;
    const size_t n = frames * channels_;
    const uint16_t bps = bytesPerSample(format_);
    buffer_.resize(n * bps);
    char* out = buffer_.data();
    switch (format_) {
        case WavFormat::U8:
            for (size_t i = 0; i < n; i++) {
                float v = std::clamp(in[i], -1.0f, 1.0f);
                out[i] = char(uint8_t(std::lround(v * 127.5f + 127.5f)));
            }
            break;
        case WavFormat::S16:
        case WavFormat::S24:
            for (size_t i = 0; i < n; i++) {
                int16_t v = int16_t(std::lround(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f));
                out[2 * i] = char(v & 0xff);
                out[2 * i + 1] = char((v >> 8) & 0xff);
            }
            break;
        case WavFormat::F32:
            std::memcpy(out, in, n * sizeof(float));
            break;
    }
    if (std::fwrite(out, 1, buffer_.size(), file_) != buffer_.size()) return false;
    dataBytes_ += buffer_.size();
    return true;
}

uint64_t WavWriter::framesWritten() const {
    const uint64_t frameBytes = uint64_t(channels_) * bytesPerSample(format_);
    return frameBytes ? dataBytes_ / frameBytes : 0;
}

void WavWriter::close() {
    if (!file_) return;
    writeHeader();
    std::fclose(file_);
    file_ = nullptr;
}

// ---- WavReader ----------------------------------------------------------------

WavReader::~WavReader() { close(); }

void WavReader::close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
}

bool WavReader::open(const std::filesystem::path& path, std::string* error) {
    close();
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        close();
        return false;
    };
    file_ = openFile(path, false);
    if (!file_) return fail("Cannot open file");

    unsigned char hdr[12];
    if (std::fread(hdr, 1, 12, file_) != 12 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0)
        return fail("Not a RIFF/WAVE file");

    bool haveFmt = false, haveData = false;
    uint16_t formatTag = 0, bits = 0, blockAlign = 0;
    uint64_t dataSize = 0;
    uint64_t offset = 12;
    centerFrequency_ = 0.0;
    while (!haveData) {
        unsigned char ch[8];
        if (!seekFile(file_, offset) || std::fread(ch, 1, 8, file_) != 8) break;
        uint32_t size = get32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            unsigned char fmt[40] = {};
            size_t len = std::min<size_t>(size, sizeof(fmt));
            if (std::fread(fmt, 1, len, file_) != len || len < 16) return fail("Invalid fmt chunk");
            formatTag = get16(fmt);
            channels_ = get16(fmt + 2);
            sampleRate_ = get32(fmt + 4);
            blockAlign = get16(fmt + 12);
            bits = get16(fmt + 14);
            if (formatTag == 0xFFFE && len >= 26) formatTag = get16(fmt + 24);  // extensible: sub format GUID
            haveFmt = true;
        } else if (std::memcmp(ch, "auxi", 4) == 0 && size >= 36) {
            unsigned char aux[36];
            if (std::fread(aux, 1, 36, file_) == 36) centerFrequency_ = double(int32_t(get32(aux + 32)));
        } else if (std::memcmp(ch, "data", 4) == 0) {
            dataOffset_ = offset + 8;
            dataSize = size;
            haveData = true;
        }
        offset += 8 + uint64_t(size) + (size & 1);
    }
    if (!haveFmt || !haveData) return fail("Missing fmt or data chunk");
    if (channels_ == 0 || blockAlign == 0) return fail("Invalid format");

    if (formatTag == 3 && bits == 32) format_ = WavFormat::F32;
    else if (formatTag == 1 && bits == 8) format_ = WavFormat::U8;
    else if (formatTag == 1 && bits == 16) format_ = WavFormat::S16;
    else if (formatTag == 1 && bits == 24) format_ = WavFormat::S24;
    else return fail("Unsupported sample format");
    bytesPerSample_ = bytesPerSample(format_);
    if (blockAlign != bytesPerSample_ * channels_) return fail("Unsupported block alignment");

    // Some writers leave the size at 0 / 0xFFFFFFFF when they were interrupted.
    std::error_code ec;
    uint64_t fileSize = std::filesystem::file_size(path, ec);
    if (!ec && (dataSize == 0 || dataSize == 0xFFFFFFFFu || dataOffset_ + dataSize > fileSize))
        dataSize = fileSize > dataOffset_ ? fileSize - dataOffset_ : 0;
    totalFrames_ = dataSize / blockAlign;
    position_ = 0;
    seekFile(file_, dataOffset_);
    return true;
}

bool WavReader::seek(uint64_t frame) {
    if (!file_) return false;
    frame = std::min(frame, totalFrames_);
    if (!seekFile(file_, dataOffset_ + frame * bytesPerSample_ * channels_)) return false;
    position_ = frame;
    return true;
}

size_t WavReader::read(float* out, size_t frames) {
    if (!file_) return 0;
    frames = size_t(std::min<uint64_t>(frames, totalFrames_ - position_));
    const size_t frameBytes = size_t(bytesPerSample_) * channels_;
    buffer_.resize(frames * frameBytes);
    size_t got = std::fread(buffer_.data(), 1, buffer_.size(), file_) / frameBytes;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(buffer_.data());
    const size_t n = got * channels_;
    switch (format_) {
        case WavFormat::U8:
            for (size_t i = 0; i < n; i++) out[i] = (float(p[i]) - 127.5f) / 127.5f;
            break;
        case WavFormat::S16:
            for (size_t i = 0; i < n; i++) out[i] = float(int16_t(get16(p + 2 * i))) / 32768.0f;
            break;
        case WavFormat::S24:
            for (size_t i = 0; i < n; i++) {
                const unsigned char* s = p + 3 * i;
                int32_t v = int32_t((uint32_t(s[0]) << 8) | (uint32_t(s[1]) << 16) | (uint32_t(s[2]) << 24)) >> 8;
                out[i] = float(v) / 8388608.0f;
            }
            break;
        case WavFormat::F32:
            std::memcpy(out, p, n * sizeof(float));
            break;
    }
    position_ += got;
    return got;
}

}  // namespace SharkSDR
