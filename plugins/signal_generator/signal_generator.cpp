// Signal generator source: a virtual band around 100 MHz with test stations
// for every built-in mode (WFM stereo, NFM, AM, USB, LSB, CW) plus noise.
// Useful to try SharkSDR without any hardware.

#include <SharkSDR/plugin.h>
#include <SharkSDR/ui/widgets.h>

#include <imgui.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using SharkSDR::Complex;

constexpr double kTwoPi = 6.283185307179586;

// ---- fast oscillators -------------------------------------------------------------

struct SineTable {
    std::array<float, 65536> v;
    SineTable() {
        for (size_t i = 0; i < v.size(); i++) v[i] = float(std::sin(kTwoPi * double(i) / 65536.0));
    }
};
const SineTable& table() {
    static SineTable t;
    return t;
}
inline float lutSin(uint32_t ph) { return table().v[ph >> 16]; }
inline float lutCos(uint32_t ph) { return table().v[(ph + 0x40000000u) >> 16]; }
inline uint32_t phaseStep(double hz, double rate) { return uint32_t(int64_t(std::llround(hz / rate * 4294967296.0))); }

// xorshift + Irwin-Hall approximation of a unit normal distribution.
struct Noise {
    uint32_t s = 2463534242u;
    float uniform() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return float(s) * (2.0f / 4294967296.0f) - 1.0f;
    }
    float gaussian() { return (uniform() + uniform() + uniform() + uniform()) * 0.8660254f; }
};

// ---- Morse keying -----------------------------------------------------------------

std::vector<bool> morseTimeline(const std::string& text) {
    static const char* codes[36] = {".-",    "-...",  "-.-.",  "-..",   ".",     "..-.",  "--.",   "....",  "..",
                                    ".---",  "-.-",   ".-..",  "--",    "-.",    "---",   ".--.",  "--.-",  ".-.",
                                    "...",   "-",     "..-",   "...-",  ".--",   "-..-",  "-.--",  "--..",  "-----",
                                    ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};
    std::vector<bool> units;
    for (char c : text) {
        if (c == ' ') {
            for (int i = 0; i < 4; i++) units.push_back(false);  // 7 units with the letter gap
            continue;
        }
        const char* code = nullptr;
        if (c >= 'A' && c <= 'Z') code = codes[c - 'A'];
        else if (c >= '0' && c <= '9') code = codes[26 + c - '0'];
        if (!code) continue;
        for (const char* p = code; *p; p++) {
            int len = *p == '-' ? 3 : 1;
            for (int i = 0; i < len; i++) units.push_back(true);
            units.push_back(false);
        }
        units.push_back(false);
        units.push_back(false);
    }
    for (int i = 0; i < 10; i++) units.push_back(false);
    return units;
}

// ---- stations ---------------------------------------------------------------------

enum class Kind { WfmStereo, Nfm, Am, Usb, Lsb, Cw };

struct Station {
    const char* label;
    const char* details;
    Kind kind;
    double frequency;
    std::atomic<float> levelDb;
    std::atomic<bool> enabled{true};

    // generator state
    uint32_t carrier = 0, tone1 = 0, tone2 = 0, pilot = 0;
    double time = 0.0;

    Station(const char* l, const char* d, Kind k, double f, float db)
        : label(l), details(d), kind(k), frequency(f), levelDb(db) {}
};

class SignalGenerator : public SharkSDR::Source {
public:
    explicit SignalGenerator(SharkSDR::Host& host) : host_(host) {
        stations_.emplace_back(new Station("WFM", "Stereo: 440 Hz left, 660 Hz right", Kind::WfmStereo, 99.7e6, -28.0f));
        stations_.emplace_back(new Station("LSB", "1 kHz tone", Kind::Lsb, 99.9e6, -42.0f));
        stations_.emplace_back(new Station("NFM", "1 kHz tone, on 3 s / off 2 s", Kind::Nfm, 100.15e6, -36.0f));
        stations_.emplace_back(new Station("AM", "700 Hz tone, 60 % modulation", Kind::Am, 100.3e6, -32.0f));
        stations_.emplace_back(new Station("USB", "Two tone 800 / 1800 Hz", Kind::Usb, 100.42e6, -40.0f));
        stations_.emplace_back(new Station("CW", "Morse \"CQ CQ DE SHARK\" at 18 WPM", Kind::Cw, 100.5e6, -44.0f));
        morse_ = morseTimeline("CQ CQ DE SHARK SHARK K");
        loadSettings();
    }

    ~SignalGenerator() override { stop(); }

    std::string name() const override { return "Signal Generator"; }

    bool start(SharkSDR::SampleSink& sink) override {
        stop();
        quit_ = false;
        thread_ = std::thread([this, &sink] { run(sink); });
        return true;
    }

    void stop() override {
        quit_ = true;
        if (thread_.joinable()) thread_.join();
    }

    double sampleRate() const override { return rate_.load(); }
    void tune(double hz) override { center_ = hz; }

    void drawSettings(bool running) override {
        static const double rates[] = {250e3, 1.024e6, 2.048e6, 2.4e6, 3.2e6};
        const double rate = rate_.load();
        SharkSDR::ui::LabeledRow("Rate");
        if (ImGui::BeginCombo("##rate", SharkSDR::ui::formatSampleRate(rate).c_str())) {
            for (double r : rates) {
                if (ImGui::Selectable(SharkSDR::ui::formatSampleRate(r).c_str(), r == rate)) {
                    rate_ = r;
                    saveSettings();
                }
            }
            ImGui::EndCombo();
        }
        float noise = noiseDb_.load();
        SharkSDR::ui::LabeledRow("Noise");
        if (SharkSDR::ui::SliderFloat("##noise", &noise, -90.0f, -20.0f, "%.0f dBFS")) {
            noiseDb_ = noise;
            saveSettings();
        }

        ImGui::TextDisabled("Stations (tune around 100 MHz):");
        if (ImGui::BeginTable("##stations", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("freq", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
            for (size_t i = 0; i < stations_.size(); i++) {
                Station& s = *stations_[i];
                ImGui::PushID(int(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool on = s.enabled;
                if (ImGui::Checkbox("##on", &on)) {
                    s.enabled = on;
                    saveSettings();
                }
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (ImGui::Selectable(SharkSDR::ui::formatFrequency(s.frequency).c_str(), false))
                    tuneTo(s);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to tune");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", s.label);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.details);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

private:
    void tuneTo(const Station& s) {
        static const char* modes[] = {"WFM", "NFM", "AM", "USB", "LSB", "CW"};
        host_.setMode(modes[int(s.kind)]);
        host_.setFrequency(s.frequency);
    }

    void loadSettings() {
        const auto& cfg = host_.config();
        rate_ = cfg.value("sampleRate", rate_.load());
        noiseDb_ = cfg.value("noiseDb", noiseDb_.load());
        if (cfg.contains("enabled") && cfg["enabled"].is_array())
            for (size_t i = 0; i < stations_.size() && i < cfg["enabled"].size(); i++)
                stations_[i]->enabled = cfg["enabled"][i].get<bool>();
    }

    void saveSettings() {
        auto& cfg = host_.config();
        cfg["sampleRate"] = rate_.load();
        cfg["noiseDb"] = noiseDb_.load();
        cfg["enabled"] = nlohmann::json::array();
        for (const auto& s : stations_) cfg["enabled"].push_back(s->enabled.load());
    }

    void run(SharkSDR::SampleSink& sink) {
        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        std::vector<Complex> buffer;
        while (!quit_) {
            const double rate = rate_.load();
            const size_t n = size_t(rate / 100.0);  // 10 ms blocks
            buffer.assign(n, Complex());
            generate(buffer.data(), n, rate);
            sink.pushSamples(buffer.data(), n);

            next += std::chrono::milliseconds(10);
            const auto now = clock::now();
            if (next < now - std::chrono::milliseconds(200)) next = now;  // we fell behind; don't try to catch up
            std::this_thread::sleep_until(next);
        }
    }

    void generate(Complex* out, size_t n, double rate) {
        const double center = center_.load();
        const double dt = 1.0 / rate;
        for (auto& sp : stations_) {
            Station& s = *sp;
            const double offset = s.frequency - center;
            if (!s.enabled || std::fabs(offset) > rate * 0.5 + 150e3) {
                s.time += double(n) * dt;
                continue;
            }
            const float a = std::pow(10.0f, s.levelDb.load() / 20.0f);
            switch (s.kind) {
                case Kind::WfmStereo: genWfm(s, out, n, rate, offset, a); break;
                case Kind::Nfm: genNfm(s, out, n, rate, offset, a); break;
                case Kind::Am: genAm(s, out, n, rate, offset, a); break;
                case Kind::Usb: genSsb(s, out, n, rate, offset, a, {800.0, 1800.0}); break;
                case Kind::Lsb: genSsb(s, out, n, rate, offset, a, {-1000.0, 0.0}); break;
                case Kind::Cw: genCw(s, out, n, rate, offset, a); break;
            }
            s.time += double(n) * dt;
        }
        // Complex white noise.
        const float sigma = std::sqrt(std::pow(10.0f, noiseDb_.load() / 10.0f) * 0.5f);
        for (size_t i = 0; i < n; i++) out[i] += Complex(noise_.gaussian() * sigma, noise_.gaussian() * sigma);
    }

    // Broadcast FM: mpx = 0.9 * [(L+R)/2 + (L-R)/2 * sin(2*pilot)] + 0.1 * sin(pilot), 75 kHz deviation.
    void genWfm(Station& s, Complex* out, size_t n, double rate, double offset, float a) {
        const uint32_t stepL = phaseStep(440.0, rate), stepR = phaseStep(660.0, rate), stepP = phaseStep(19000.0, rate);
        const float base = float(offset / rate * 4294967296.0);
        const float dev = float(75000.0 / rate * 4294967296.0);
        for (size_t i = 0; i < n; i++) {
            const float l = 0.8f * lutSin(s.tone1);
            const float r = 0.8f * lutSin(s.tone2);
            const float mpx = 0.9f * (0.5f * (l + r) + 0.5f * (l - r) * lutSin(s.pilot * 2u)) + 0.1f * lutSin(s.pilot);
            s.carrier += uint32_t(int64_t(base + dev * mpx));
            out[i] += Complex(a * lutCos(s.carrier), a * lutSin(s.carrier));
            s.tone1 += stepL;
            s.tone2 += stepR;
            s.pilot += stepP;
        }
    }

    // Narrow FM, 1 kHz tone at 2.5 kHz deviation, transmitting 3 s out of 5 s.
    void genNfm(Station& s, Complex* out, size_t n, double rate, double offset, float a) {
        const uint32_t stepT = phaseStep(1000.0, rate);
        const float base = float(offset / rate * 4294967296.0);
        const float dev = float(2500.0 / rate * 4294967296.0);
        const double dt = 1.0 / rate;
        for (size_t i = 0; i < n; i++) {
            const bool on = std::fmod(s.time + double(i) * dt, 5.0) < 3.0;
            s.carrier += uint32_t(int64_t(base + dev * lutSin(s.tone1)));
            s.tone1 += stepT;
            if (on) out[i] += Complex(a * lutCos(s.carrier), a * lutSin(s.carrier));
        }
    }

    void genAm(Station& s, Complex* out, size_t n, double rate, double offset, float a) {
        const uint32_t stepC = phaseStep(offset, rate), stepT = phaseStep(700.0, rate);
        for (size_t i = 0; i < n; i++) {
            const float env = a * (1.0f + 0.6f * lutSin(s.tone1));
            out[i] += Complex(env * lutCos(s.carrier), env * lutSin(s.carrier));
            s.carrier += stepC;
            s.tone1 += stepT;
        }
    }

    // Single sideband = sum of tones at offset + audio frequency (negative for LSB).
    void genSsb(Station& s, Complex* out, size_t n, double rate, double offset, float a, std::array<double, 2> tones) {
        const uint32_t step1 = phaseStep(offset + tones[0], rate);
        const uint32_t step2 = phaseStep(offset + tones[1], rate);
        const bool two = tones[1] != 0.0;
        const float g = two ? a * 0.5f : a;
        for (size_t i = 0; i < n; i++) {
            Complex v(lutCos(s.tone1), lutSin(s.tone1));
            if (two) v += Complex(lutCos(s.tone2), lutSin(s.tone2));
            out[i] += v * g;
            s.tone1 += step1;
            s.tone2 += step2;
        }
    }

    void genCw(Station& s, Complex* out, size_t n, double rate, double offset, float a) {
        const uint32_t stepC = phaseStep(offset, rate);
        const double unit = 1.2 / 18.0;  // 18 WPM
        const double ramp = 0.005;
        const double dt = 1.0 / rate;
        const double period = unit * double(morse_.size());
        for (size_t i = 0; i < n; i++) {
            const double t = std::fmod(s.time + double(i) * dt, period);
            const size_t u = size_t(t / unit);
            float env = 0.0f;
            if (morse_[u]) {
                // raised cosine edges against key clicks
                const double inUnit = t - double(u) * unit;
                const bool startEdge = u == 0 || !morse_[u - 1];
                const bool endEdge = u + 1 >= morse_.size() || !morse_[u + 1];
                env = 1.0f;
                if (startEdge && inUnit < ramp) env = float(0.5 - 0.5 * std::cos(kTwoPi * 0.5 * inUnit / ramp));
                if (endEdge && unit - inUnit < ramp) env = float(0.5 - 0.5 * std::cos(kTwoPi * 0.5 * (unit - inUnit) / ramp));
            }
            out[i] += Complex(a * env * lutCos(s.carrier), a * env * lutSin(s.carrier));
            s.carrier += stepC;
        }
    }

    SharkSDR::Host& host_;
    std::vector<std::unique_ptr<Station>> stations_;
    std::vector<bool> morse_;
    std::atomic<double> rate_{2.4e6};
    std::atomic<double> center_{100e6};
    std::atomic<float> noiseDb_{-50.0f};
    Noise noise_;
    std::thread thread_;
    std::atomic<bool> quit_{false};
};

class SignalGeneratorPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        source_ = std::make_shared<SignalGenerator>(host);
        host.registerSource(source_);
    }
    void shutdown() override {
        if (source_) source_->stop();
        source_.reset();
    }

private:
    std::shared_ptr<SignalGenerator> source_;
};

}  // namespace

SHARKSDR_PLUGIN(SignalGeneratorPlugin, "Signal Generator", "1.0.0", "SharkSDR",
                "Virtual band around 100 MHz with WFM stereo, NFM, AM, SSB and CW test stations.")
