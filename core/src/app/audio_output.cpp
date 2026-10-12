#include "app/audio_output.h"

#include <SharkSDR/log.h>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include <miniaudio.h>

#include <algorithm>
#include <cstring>

namespace SharkSDR {

struct AudioOutput::Impl {
    ma_context context{};
    bool contextReady = false;
    ma_device device{};
    bool deviceOpen = false;
    std::vector<ma_device_info> playbackDevices;
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {
    ring_.resize(size_t(kSampleRate));  // 1 second
}

AudioOutput::~AudioOutput() { shutdown(); }

bool AudioOutput::init() {
    if (impl_->contextReady) return true;
    if (ma_context_init(nullptr, 0, nullptr, &impl_->context) != MA_SUCCESS) {
        logError("Audio", "Cannot initialise the audio backend");
        return false;
    }
    impl_->contextReady = true;
    logInfo("Audio", std::string("Backend: ") + ma_get_backend_name(impl_->context.backend));
    refreshDevices();
    return true;
}

void AudioOutput::shutdown() {
    close();
    if (impl_->contextReady) ma_context_uninit(&impl_->context);
    impl_->contextReady = false;
}

void AudioOutput::refreshDevices() {
    deviceNames_.clear();
    impl_->playbackDevices.clear();
    if (!impl_->contextReady) return;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&impl_->context, &infos, &count, nullptr, nullptr) != MA_SUCCESS) return;
    for (ma_uint32 i = 0; i < count; i++) {
        impl_->playbackDevices.push_back(infos[i]);
        deviceNames_.push_back(infos[i].name);
    }
}

bool AudioOutput::isOpen() const { return impl_->deviceOpen; }

namespace {
void deviceCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    static_cast<AudioOutput*>(device->pUserData)->render(static_cast<Stereo*>(output), frames);
}
}  // namespace

void AudioOutput::render(Stereo* out, size_t frames) {
    const size_t target = size_t(kSampleRate * 0.06);   // 60 ms cushion
    const size_t maxFill = size_t(kSampleRate * 0.25);  // drop when latency grows above 250 ms
    size_t avail = ring_.readAvailable();
    if (!primed_.load()) {
        if (avail < target) {
            std::memset(static_cast<void*>(out), 0, frames * sizeof(Stereo));
            return;
        }
        primed_ = true;
    }
    if (avail > maxFill) ring_.skip(avail - target);
    size_t got = ring_.read(out, frames);
    if (got < frames) {
        std::memset(static_cast<void*>(out + got), 0, (frames - got) * sizeof(Stereo));
        primed_ = false;
        underruns_++;
    }
}

bool AudioOutput::open(const std::string& deviceName) {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    if (!impl_->contextReady && !init()) return false;
    if (impl_->deviceOpen) {
        ma_device_uninit(&impl_->device);
        impl_->deviceOpen = false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = ma_uint32(kSampleRate);
    cfg.dataCallback = deviceCallback;
    cfg.pUserData = this;
    cfg.periodSizeInMilliseconds = 10;

    currentDevice_.clear();
    for (const auto& info : impl_->playbackDevices) {
        if (!deviceName.empty() && deviceName == info.name) {
            cfg.playback.pDeviceID = &info.id;
            currentDevice_ = deviceName;
            break;
        }
    }

    if (ma_device_init(&impl_->context, &cfg, &impl_->device) != MA_SUCCESS) {
        logError("Audio", "Cannot open audio device '" + (deviceName.empty() ? std::string("default") : deviceName) + "'");
        return false;
    }
    impl_->deviceOpen = true;
    primed_ = false;
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        logError("Audio", "Cannot start audio device");
        ma_device_uninit(&impl_->device);
        impl_->deviceOpen = false;
        return false;
    }
    logInfo("Audio", "Output: " + std::string(impl_->device.playback.name));
    return true;
}

void AudioOutput::close() {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    if (impl_->deviceOpen) ma_device_uninit(&impl_->device);
    impl_->deviceOpen = false;
}

void AudioOutput::write(const Stereo* frames, size_t count) { ring_.write(frames, count); }

float AudioOutput::bufferedMs() const { return float(ring_.readAvailable() * 1000.0 / kSampleRate); }

}  // namespace SharkSDR
