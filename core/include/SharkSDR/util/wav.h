#pragma once

#include <SharkSDR/export.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace SharkSDR {

enum class WavFormat { U8, S16, S24, F32 };

// Streaming WAV writer (PCM 8/16 bit or 32 bit float). Samples are passed as
// interleaved floats in [-1, 1].
class SHARKSDR_API WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool open(const std::filesystem::path& path, uint32_t sampleRate, uint16_t channels, WavFormat format);
    bool write(const float* interleaved, size_t frames);
    void close();

    bool isOpen() const { return file_ != nullptr; }
    uint64_t bytesWritten() const { return dataBytes_; }
    uint64_t framesWritten() const;

private:
    void writeHeader();

    std::FILE* file_ = nullptr;
    uint32_t sampleRate_ = 0;
    uint16_t channels_ = 0;
    WavFormat format_ = WavFormat::S16;
    uint64_t dataBytes_ = 0;
    std::string buffer_;
};

// WAV reader (PCM 8/16/24 bit, 32 bit float, WAVE_FORMAT_EXTENSIBLE). Also
// reads the center frequency stored by SDR# in the "auxi" chunk.
class SHARKSDR_API WavReader {
public:
    WavReader() = default;
    ~WavReader();
    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;

    bool open(const std::filesystem::path& path, std::string* error = nullptr);
    void close();
    bool isOpen() const { return file_ != nullptr; }

    uint32_t sampleRate() const { return sampleRate_; }
    uint16_t channels() const { return channels_; }
    WavFormat format() const { return format_; }
    uint64_t totalFrames() const { return totalFrames_; }
    uint64_t position() const { return position_; }
    double centerFrequency() const { return centerFrequency_; }  // 0 if unknown

    // Reads up to `frames` frames as interleaved floats. Returns frames read.
    size_t read(float* interleaved, size_t frames);
    bool seek(uint64_t frame);

private:
    std::FILE* file_ = nullptr;
    uint32_t sampleRate_ = 0;
    uint16_t channels_ = 0;
    uint16_t bytesPerSample_ = 0;
    WavFormat format_ = WavFormat::S16;
    uint64_t dataOffset_ = 0;
    uint64_t totalFrames_ = 0;
    uint64_t position_ = 0;
    double centerFrequency_ = 0.0;
    std::string buffer_;
};

}  // namespace SharkSDR
