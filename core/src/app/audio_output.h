#pragma once

#include <SharkSDR/dsp/ringbuffer.h>
#include <SharkSDR/types.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SharkSDR {

// Sound card output (miniaudio). Audio is written by the DSP thread into a
// ring buffer that the device callback drains.
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    bool init();
    void shutdown();

    void refreshDevices();
    const std::vector<std::string>& deviceNames() const { return deviceNames_; }

    // Opens the device with the given name ("" = system default).
    bool open(const std::string& deviceName);
    void close();
    const std::string& currentDevice() const { return currentDevice_; }
    bool isOpen() const;

    double sampleRate() const { return kSampleRate; }

    // DSP thread.
    void write(const Stereo* frames, size_t count);

    float bufferedMs() const;
    uint64_t underruns() const { return underruns_.load(); }

    // Device callback: fills `frames` output frames from the ring buffer.
    void render(Stereo* out, size_t frames);

    static constexpr double kSampleRate = 48000.0;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
    std::vector<std::string> deviceNames_;
    std::string currentDevice_;
    dsp::RingBuffer<Stereo> ring_;
    std::atomic<bool> primed_{false};
    std::atomic<uint64_t> underruns_{0};
    std::mutex deviceMutex_;
};

}  // namespace SharkSDR
