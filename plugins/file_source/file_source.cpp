// IQ file playback source. Supports WAV (8/16/24 bit PCM, 32 bit float; e.g.
// recordings from SharkSDR, SDR# and SDR++) and raw interleaved IQ files
// (.cu8 from rtl_sdr, .cs8, .cs16, .cf32).

#include <SharkSDR/paths.h>
#include <SharkSDR/plugin.h>
#include <SharkSDR/ui/widgets.h>
#include <SharkSDR/util/wav.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {

using SharkSDR::Complex;
namespace fs = std::filesystem;

enum class RawFormat { CU8, CS8, CS16, CF32 };
const char* const kRawFormatNames[] = {"Unsigned 8 bit (cu8, rtl_sdr)", "Signed 8 bit (cs8, HackRF)",
                                       "Signed 16 bit (cs16)", "Float 32 bit (cf32)"};
const size_t kRawFormatBytes[] = {2, 2, 4, 8};

// ---- readers --------------------------------------------------------------------------

class IqReader {
public:
    virtual ~IqReader() = default;
    virtual size_t read(Complex* out, size_t frames) = 0;
    virtual bool seek(uint64_t frame) = 0;
    virtual uint64_t totalFrames() const = 0;
};

class WavIqReader : public IqReader {
public:
    bool open(const fs::path& path, std::string* error) {
        if (!wav_.open(path, error)) return false;
        if (wav_.channels() != 2) {
            if (error) *error = "WAV file must have 2 channels (I and Q)";
            return false;
        }
        return true;
    }
    size_t read(Complex* out, size_t frames) override {
        buffer_.resize(frames * 2);
        size_t got = wav_.read(buffer_.data(), frames);
        for (size_t i = 0; i < got; i++) out[i] = Complex(buffer_[2 * i], buffer_[2 * i + 1]);
        return got;
    }
    bool seek(uint64_t frame) override { return wav_.seek(frame); }
    uint64_t totalFrames() const override { return wav_.totalFrames(); }
    const SharkSDR::WavReader& wav() const { return wav_; }

private:
    SharkSDR::WavReader wav_;
    std::vector<float> buffer_;
};

class RawIqReader : public IqReader {
public:
    ~RawIqReader() override {
        if (file_) std::fclose(file_);
    }
    bool open(const fs::path& path, RawFormat format, std::string* error) {
#if defined(_WIN32)
        file_ = _wfopen(path.c_str(), L"rb");
#else
        file_ = std::fopen(path.c_str(), "rb");
#endif
        if (!file_) {
            if (error) *error = "Cannot open file";
            return false;
        }
        format_ = format;
        std::error_code ec;
        total_ = fs::file_size(path, ec) / kRawFormatBytes[int(format)];
        return true;
    }
    size_t read(Complex* out, size_t frames) override {
        const size_t fb = kRawFormatBytes[int(format_)];
        buffer_.resize(frames * fb);
        const size_t got = std::fread(buffer_.data(), fb, frames, file_);
        const unsigned char* p = buffer_.data();
        for (size_t i = 0; i < got; i++) {
            switch (format_) {
                case RawFormat::CU8:
                    out[i] = Complex((float(p[2 * i]) - 127.5f) / 127.5f, (float(p[2 * i + 1]) - 127.5f) / 127.5f);
                    break;
                case RawFormat::CS8:
                    out[i] = Complex(float(int8_t(p[2 * i])) / 128.0f, float(int8_t(p[2 * i + 1])) / 128.0f);
                    break;
                case RawFormat::CS16: {
                    int16_t re = int16_t(p[4 * i] | (p[4 * i + 1] << 8));
                    int16_t im = int16_t(p[4 * i + 2] | (p[4 * i + 3] << 8));
                    out[i] = Complex(float(re) / 32768.0f, float(im) / 32768.0f);
                    break;
                }
                case RawFormat::CF32: {
                    float v[2];
                    std::memcpy(v, p + 8 * i, 8);
                    out[i] = Complex(v[0], v[1]);
                    break;
                }
            }
        }
        return got;
    }
    bool seek(uint64_t frame) override {
        const uint64_t off = frame * kRawFormatBytes[int(format_)];
#if defined(_WIN32)
        return _fseeki64(file_, int64_t(off), SEEK_SET) == 0;
#else
        return fseeko(file_, off_t(off), SEEK_SET) == 0;
#endif
    }
    uint64_t totalFrames() const override { return total_; }

private:
    std::FILE* file_ = nullptr;
    RawFormat format_ = RawFormat::CU8;
    uint64_t total_ = 0;
    std::vector<unsigned char> buffer_;
};

// Frequency embedded in a file name, e.g. "baseband_100000000Hz_...wav" (SDR++),
// "SDRSharp_20240101_120000Z_145500000Hz_IQ.wav" (SDR#) or "..._433.92MHz.cu8".
double frequencyFromName(const std::string& name) {
    std::smatch m;
    static const std::regex hz(R"((\d{4,12})\s*Hz)", std::regex::icase);
    static const std::regex mhz(R"((\d+(?:\.\d+)?)\s*MHz)", std::regex::icase);
    if (std::regex_search(name, m, mhz)) return std::stod(m[1]) * 1e6;
    if (std::regex_search(name, m, hz)) return std::stod(m[1]);
    return 0.0;
}

std::string formatDuration(double s) {
    char buf[32];
    const int t = int(s);
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    return buf;
}

// ---- source -------------------------------------------------------------------------

class FileSource : public SharkSDR::Source {
public:
    explicit FileSource(SharkSDR::Host& host) : host_(host) {
        const auto& cfg = host_.config();
        path_ = cfg.value("path", std::string());
        rawFormat_ = std::clamp(cfg.value("rawFormat", 0), 0, 3);
        rawRate_ = cfg.value("rawRate", 2.4e6);
        manualCenter_ = cfg.value("center", 100e6);
        loop_ = cfg.value("loop", true);
        probe();
    }
    ~FileSource() override { stop(); }

    std::string name() const override { return "File"; }
    bool tunable() const override { return false; }
    void tune(double) override {}
    double frequency() const override { return center_.load(); }
    double sampleRate() const override { return rate_.load(); }
    std::string lastError() const override { return error_; }

    bool start(SharkSDR::SampleSink& sink) override {
        stop();
        std::unique_ptr<IqReader> reader = openReader(&error_);
        if (!reader) return false;
        position_ = 0;
        total_ = reader->totalFrames();
        quit_ = false;
        thread_ = std::thread([this, &sink, r = std::move(reader)]() mutable { run(sink, *r); });
        return true;
    }

    void stop() override {
        quit_ = true;
        if (thread_.joinable()) thread_.join();
    }

    void drawSettings(bool running) override {
        ImGui::BeginDisabled(running);
        SharkSDR::ui::LabeledRow("File");
        if (SharkSDR::ui::PathPicker("##file", &path_, false, ".wav;.cu8;.cs8;.cs16;.cf32;.raw;.iq;.bin")) {
            probe();
            save();
        }
        if (!isWav()) {
            SharkSDR::ui::LabeledRow("Format");
            if (ImGui::Combo("##fmt", &rawFormat_, kRawFormatNames, 4)) {
                probe();
                save();
            }
            SharkSDR::ui::LabeledRow("Rate");
            if (SharkSDR::ui::InputFrequency("##rawrate", &rawRate_)) {
                probe();
                save();
            }
        }
        SharkSDR::ui::LabeledRow("Center");
        if (autoCenter_) {
            // Detected from the WAV "auxi" chunk or the file name.
            ImGui::BeginDisabled();
            double c = center_.load();
            SharkSDR::ui::InputFrequency("##center_auto", &c);
            ImGui::EndDisabled();
            SharkSDR::ui::LabeledRow("");
            ImGui::TextDisabled("Read from the file");
        } else {
            double c = manualCenter_;
            if (SharkSDR::ui::InputFrequency("##center", &c)) {
                manualCenter_ = c;
                center_ = c;
                save();
            }
        }
        ImGui::EndDisabled();

        bool loop = loop_.load();
        if (ImGui::Checkbox("Loop", &loop)) {
            loop_ = loop;
            save();
        }

        if (!info_.empty()) ImGui::TextDisabled("%s", info_.c_str());
        if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error_.c_str());

        // Position / seek bar.
        const double rate = rate_.load();
        const uint64_t total = total_.load();
        if (running && total > 0 && rate > 0) {
            float pos = float(double(position_.load()) / double(total));
            std::string label = formatDuration(double(position_.load()) / rate) + " / " + formatDuration(double(total) / rate);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat("##pos", &pos, 0.0f, 1.0f, label.c_str(), ImGuiSliderFlags_NoInput))
                seek_ = int64_t(double(pos) * double(total));
        }
    }

private:
    bool isWav() const {
        std::string ext = SharkSDR::pathToUtf8(SharkSDR::utf8ToPath(path_).extension());
        for (auto& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return ext == ".wav";
    }

    std::unique_ptr<IqReader> openReader(std::string* error) {
        if (path_.empty()) {
            if (error) *error = "No file selected";
            return nullptr;
        }
        const fs::path p = SharkSDR::utf8ToPath(path_);
        if (isWav()) {
            auto r = std::make_unique<WavIqReader>();
            if (!r->open(p, error)) return nullptr;
            return r;
        }
        auto r = std::make_unique<RawIqReader>();
        if (!r->open(p, RawFormat(rawFormat_), error)) return nullptr;
        return r;
    }

    // Reads the file header to show its properties before playing.
    void probe() {
        info_.clear();
        error_.clear();
        autoCenter_ = false;
        total_ = 0;
        if (path_.empty()) return;
        const fs::path p = SharkSDR::utf8ToPath(path_);
        double rate = rawRate_;
        double center = 0.0;
        if (isWav()) {
            WavIqReader r;
            if (!r.open(p, &error_)) return;
            rate = r.wav().sampleRate();
            center = r.wav().centerFrequency();
            total_ = r.totalFrames();
        } else {
            RawIqReader r;
            if (!r.open(p, RawFormat(rawFormat_), &error_)) return;
            total_ = r.totalFrames();
        }
        if (center <= 0.0) center = frequencyFromName(SharkSDR::pathToUtf8(p.filename()));
        autoCenter_ = center > 0.0;
        center_ = autoCenter_ ? center : manualCenter_;
        rate_ = rate;
        info_ = SharkSDR::ui::formatSampleRate(rate) + ", " + formatDuration(double(total_.load()) / std::max(1.0, rate));
    }

    void save() {
        auto& cfg = host_.config();
        cfg["path"] = path_;
        cfg["rawFormat"] = rawFormat_;
        cfg["rawRate"] = rawRate_;
        cfg["center"] = manualCenter_;
        cfg["loop"] = loop_.load();
    }

    void run(SharkSDR::SampleSink& sink, IqReader& reader) {
        using clock = std::chrono::steady_clock;
        std::vector<Complex> buffer;
        auto next = clock::now();
        while (!quit_) {
            const int64_t seekTo = seek_.exchange(-1);
            if (seekTo >= 0 && reader.seek(uint64_t(seekTo))) position_ = uint64_t(seekTo);

            const double rate = rate_.load();
            const size_t n = std::max<size_t>(1, size_t(rate / 50.0));  // 20 ms blocks
            buffer.resize(n);
            size_t got = reader.read(buffer.data(), n);
            if (got < n) {
                if (loop_ && reader.seek(0)) {
                    position_ = 0;
                    got += reader.read(buffer.data() + got, n - got);
                } else {
                    if (got > 0) sink.pushSamples(buffer.data(), got);
                    sink.sourceStopped("End of file");
                    return;
                }
            }
            position_ += got;
            sink.pushSamples(buffer.data(), got);

            next += std::chrono::microseconds(int64_t(double(got) / rate * 1e6));
            const auto now = clock::now();
            if (next < now - std::chrono::milliseconds(200)) next = now;
            std::this_thread::sleep_until(next);
        }
    }

    SharkSDR::Host& host_;
    std::string path_;
    int rawFormat_ = 0;
    double rawRate_ = 2.4e6;
    double manualCenter_ = 100e6;
    bool autoCenter_ = false;
    std::atomic<bool> loop_{true};  // read by the playback thread
    std::string info_;
    std::string error_;

    std::atomic<double> rate_{2.4e6};
    std::atomic<double> center_{100e6};
    std::atomic<uint64_t> position_{0};
    std::atomic<uint64_t> total_{0};
    std::atomic<int64_t> seek_{-1};
    std::thread thread_;
    std::atomic<bool> quit_{false};
};

class FileSourcePlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        source_ = std::make_shared<FileSource>(host);
        host.registerSource(source_);
    }
    void shutdown() override {
        if (source_) source_->stop();
        source_.reset();
    }

private:
    std::shared_ptr<FileSource> source_;
};

}  // namespace

SHARKSDR_PLUGIN(FileSourcePlugin, "File Source", "1.0.0", "SharkSDR",
                "Plays IQ recordings: WAV (SharkSDR, SDR#, SDR++) and raw cu8/cs8/cs16/cf32 files.")
