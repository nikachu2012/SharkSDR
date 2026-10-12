// Recorder: saves the demodulated audio or the raw baseband IQ to WAV files.
// IQ recordings embed the center frequency in the file name so that the File
// source can play them back at the right frequency.

#include <SharkSDR/paths.h>
#include <SharkSDR/plugin.h>
#include <SharkSDR/ui/widgets.h>
#include <SharkSDR/util/wav.h>

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

enum class Mode { Audio = 0, Baseband = 1 };

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

std::string formatBytes(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.2f GB", double(b) / double(1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", double(b) / double(1ull << 20));
    else std::snprintf(buf, sizeof(buf), "%.0f kB", double(b) / 1024.0);
    return buf;
}

class Recorder {
public:
    explicit Recorder(SharkSDR::Host& host) : host_(host) {
        const auto& c = host_.config();
        mode_ = c.value("mode", 0);
        format_ = c.value("format", 0);
        folder_ = c.value("folder", SharkSDR::pathToUtf8(fs::path(SharkSDR::utf8ToPath(host_.dataDirectory())) / "recordings"));
    }
    ~Recorder() { stopRecording(); }

    void drawPanel() {
        const bool rec = recording_.load();
        ImGui::BeginDisabled(rec);
        ImGui::RadioButton("Audio", &mode_, int(Mode::Audio));
        SharkSDR::ui::SameLineColumn();
        ImGui::RadioButton("Baseband IQ", &mode_, int(Mode::Baseband));
        SharkSDR::ui::LabeledRow("Format");
        const char* formats[] = {"16 bit PCM", "32 bit float", "8 bit PCM"};
        ImGui::Combo("##format", &format_, formats, mode_ == int(Mode::Baseband) ? 3 : 2);
        SharkSDR::ui::LabeledRow("Folder");
        SharkSDR::ui::PathPicker("##folder", &folder_, true);
        ImGui::EndDisabled();

        const float w = ImGui::GetContentRegionAvail().x;
        if (!rec) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.16f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.22f, 0.22f, 1.0f));
            ImGui::BeginDisabled(!host_.isRunning());
            if (ImGui::Button("Record", ImVec2(w, 0))) startRecording();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+R");
            ImGui::EndDisabled();
            ImGui::PopStyleColor(2);
            if (!host_.isRunning()) ImGui::TextDisabled("Start the radio to record");
        } else {
            if (ImGui::Button("Stop recording", ImVec2(w, 0))) stopRecording();
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
            const float blink = std::fmod(float(secs), 1.0f) < 0.5f ? 1.0f : 0.35f;
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, blink), "REC");
            ImGui::SameLine();
            ImGui::Text("%02d:%02d:%02d  %s", int(secs) / 3600, (int(secs) / 60) % 60, int(secs) % 60,
                        formatBytes(bytes_.load()).c_str());
            ImGui::TextDisabled("%s", fileName_.c_str());
            if (dropped_.load() > 0) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Dropped %llu samples (disk too slow)",
                                                        (unsigned long long)dropped_.load());
        }
        if (!lastError_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", lastError_.c_str());
        save();
    }

    // Called every frame, even when the panel is collapsed.
    void update() {
        // Ctrl+R toggles the recording from anywhere in the application.
        if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R)) {
            if (recording_) stopRecording();
            else if (host_.isRunning()) startRecording();
        }
        // The recording ends when the radio stops or the file cannot be written.
        if (recording_ && (!host_.isRunning() || failed_.load())) stopRecording();
    }

    void stopRecording() {
        if (!recording_.exchange(false)) return;
        host_.removeTap(tap_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (writer_.joinable()) writer_.join();
        host_.log(SharkSDR::LogLevel::Info, "Saved " + fileName_ + " (" + formatBytes(bytes_.load()) + ")");
    }

private:
    void save() {
        auto& c = host_.config();
        c["mode"] = mode_;
        c["format"] = format_;
        c["folder"] = folder_;
    }

    void startRecording() {
        lastError_.clear();
        std::error_code ec;
        const fs::path dir = SharkSDR::utf8ToPath(folder_);
        fs::create_directories(dir, ec);
        const bool iq = mode_ == int(Mode::Baseband);
        const long long freq = iq ? (long long)host_.centerFrequency() : (long long)host_.frequency();
        fileName_ = "SharkSDR_" + std::string(iq ? "IQ_" : "AF_") + timestamp() + "_" + std::to_string(freq) + "Hz" +
                    (iq ? "" : "_" + host_.mode()) + ".wav";
        const uint32_t rate = uint32_t(iq ? host_.sampleRate() : 48000.0);
        SharkSDR::WavFormat fmt = format_ == 1 ? SharkSDR::WavFormat::F32 : format_ == 2 ? SharkSDR::WavFormat::U8 : SharkSDR::WavFormat::S16;
        if (!wav_.open(dir / SharkSDR::utf8ToPath(fileName_), rate, 2, fmt)) {
            lastError_ = "Cannot create " + SharkSDR::pathToUtf8(dir / SharkSDR::utf8ToPath(fileName_));
            return;
        }
        pending_.clear();
        quit_ = false;
        failed_ = false;
        bytes_ = 0;
        dropped_ = 0;
        recordRate_ = double(rate);
        started_ = std::chrono::steady_clock::now();
        writer_ = std::thread([this] { writerLoop(); });
        recording_ = true;

        if (iq) {
            tap_ = host_.addIqTap([this](const SharkSDR::Complex* s, size_t n, double sr, double) {
                if (sr != recordRate_) {
                    failed_ = true;  // sample rate changed: the WAV header would be wrong
                    return;
                }
                append(reinterpret_cast<const float*>(s), n * 2);
            });
        } else {
            tap_ = host_.addAudioTap([this](const SharkSDR::Stereo* f, size_t n, double) {
                append(reinterpret_cast<const float*>(f), n * 2);
            });
        }
    }

    // DSP thread: queue samples for the writer thread.
    void append(const float* data, size_t count) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t limit = size_t(recordRate_ * 2.0 * 4.0);  // 4 seconds of backlog
            if (pending_.size() + count > limit) {
                dropped_ += count / 2;
                return;
            }
            pending_.insert(pending_.end(), data, data + count);
        }
        cv_.notify_one();
    }

    void writerLoop() {
        std::vector<float> local;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return quit_ || !pending_.empty(); });
                local.swap(pending_);
                pending_.clear();
            }
            if (!local.empty()) {
                if (!wav_.write(local.data(), local.size() / 2)) failed_ = true;
                bytes_ = wav_.bytesWritten();
                local.clear();
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (quit_ && pending_.empty()) break;
        }
        wav_.close();
    }

    SharkSDR::Host& host_;
    int mode_ = 0;
    int format_ = 0;
    std::string folder_;
    std::string fileName_;
    std::string lastError_;

    SharkSDR::WavWriter wav_;
    SharkSDR::TapHandle tap_ = 0;
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<float> pending_;
    bool quit_ = false;
    double recordRate_ = 48000.0;
    std::atomic<bool> recording_{false};
    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> dropped_{0};
    std::chrono::steady_clock::time_point started_;
};

class RecorderPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        recorder_ = std::make_unique<Recorder>(host);
        host.registerPanel("Recorder", [this] { recorder_->drawPanel(); });
        host.registerFrameCallback([this] { recorder_->update(); });
    }
    void shutdown() override { recorder_.reset(); }

private:
    std::unique_ptr<Recorder> recorder_;
};

}  // namespace

SHARKSDR_PLUGIN(RecorderPlugin, "Recorder", "1.0.0", "SharkSDR", "Records demodulated audio or baseband IQ to WAV files.")
