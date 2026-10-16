// rtl_tcp client: receives IQ from an RTL-SDR dongle served by `rtl_tcp`
// (part of the osmocom rtl-sdr tools) on this or another machine. This gives
// RTL-SDR support on every platform without linking any driver.

#include <SharkSDR/plugin.h>
#include <SharkSDR/ui/widgets.h>
#include <SharkSDR/util/net.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using SharkSDR::Complex;

const double kSampleRates[] = {250e3, 1.024e6, 1.4e6, 1.8e6, 1.92e6, 2.048e6, 2.4e6, 2.56e6, 2.88e6, 3.2e6};

// Tuner gain steps in tenths of dB (same tables as librtlsdr).
const std::vector<int> kR820TGains = {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
                                      280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
const std::vector<int> kE4000Gains = {-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420};
const std::vector<int> kFc0012Gains = {-99, -40, 71, 179, 192};
const std::vector<int> kFc0013Gains = {-99, -73, -65, -63, -60, -58, -54, 58,  61,  63,  65, 67,
                                       68,  70,  71,  179, 181, 182, 184, 186, 188, 191, 197};
const std::vector<int> kFc2580Gains = {0};

// Index of the gain step closest to `db`.
int nearestGain(const std::vector<int>& gains, float db) {
    int best = 0;
    for (int i = 1; i < int(gains.size()); i++)
        if (std::fabs(gains[size_t(i)] - db * 10.0f) < std::fabs(gains[size_t(best)] - db * 10.0f)) best = i;
    return best;
}

const char* tunerName(uint32_t type) {
    switch (type) {
        case 1: return "E4000";
        case 2: return "FC0012";
        case 3: return "FC0013";
        case 4: return "FC2580";
        case 5: return "R820T";
        case 6: return "R828D";
        default: return "unknown";
    }
}

enum Command : uint8_t {
    kSetFrequency = 0x01,
    kSetSampleRate = 0x02,
    kSetGainMode = 0x03,
    kSetGain = 0x04,
    kSetPpm = 0x05,
    kSetAgc = 0x08,
    kSetDirectSampling = 0x09,
    kSetOffsetTuning = 0x0a,
    kSetBiasTee = 0x0e,
};

class RtlTcpSource : public SharkSDR::Source {
public:
    explicit RtlTcpSource(SharkSDR::Host& host) : host_(host) {
        for (int i = 0; i < 256; i++) lut_[size_t(i)] = (float(i) - 127.5f) / 127.5f;
        const auto& c = host_.config();
        hostName_ = c.value("host", std::string("127.0.0.1"));
        port_ = c.value("port", 1234);
        rate_ = c.value("sampleRate", 2.4e6);
        // "manualGain" is the name used by older versions (inverse meaning).
        tunerAgc_ = c.contains("tunerAgc") ? c.value("tunerAgc", true) : !c.value("manualGain", false);
        gainIndex_ = c.value("gainIndex", 14);
        ppm_ = c.value("ppm", 0);
        rtlAgc_ = c.value("rtlAgc", false);
        directSampling_ = c.value("directSampling", 0);
        biasTee_ = c.value("biasTee", false);
        offsetTuning_ = c.value("offsetTuning", false);
        tunerType_ = c.value("tunerType", tunerType_);
    }
    ~RtlTcpSource() override { stop(); }

    std::string name() const override { return "RTL-TCP"; }
    double sampleRate() const override { return rate_.load(); }
    // The RTL2832U's decimation filter rolls off above about 80 % of the rate
    // (3 dB down at +-0.99 MHz, 7.5 dB at the edge with 2.4 MS/s).
    double usableSpectrumRatio() const override { return 0.8; }
    std::string lastError() const override { return error_; }

    bool start(SharkSDR::SampleSink& sink) override {
        stop();
        error_.clear();
        if (!socket_.connect(hostName_, port_, 3000, &error_)) {
            error_ = "rtl_tcp " + hostName_ + ":" + std::to_string(port_) + ": " + error_;
            return false;
        }
        // 12 byte greeting: "RTL0", tuner type, number of gain steps (big endian).
        unsigned char hdr[12];
        if (!socket_.receiveAll(hdr, sizeof(hdr), 3000) || hdr[0] != 'R' || hdr[1] != 'T' || hdr[2] != 'L' || hdr[3] != '0') {
            error_ = "Not an rtl_tcp server (no RTL0 header)";
            socket_.close();
            return false;
        }
        tunerType_ = (uint32_t(hdr[4]) << 24) | (uint32_t(hdr[5]) << 16) | (uint32_t(hdr[6]) << 8) | hdr[7];
        host_.log(SharkSDR::LogLevel::Info, std::string("Connected, tuner ") + tunerName(tunerType_));
        host_.config()["tunerType"] = tunerType_;  // so the gain list is right before the next connection
        socket_.setReceiveBufferSize(1 << 20);

        connected_ = true;
        send(kSetSampleRate, uint32_t(rate_.load()));
        send(kSetFrequency, uint32_t(center_.load()));
        send(kSetPpm, uint32_t(ppm_));
        send(kSetAgc, rtlAgc_ ? 1 : 0);
        send(kSetDirectSampling, uint32_t(directSampling_));
        send(kSetOffsetTuning, offsetTuning_ ? 1 : 0);
        send(kSetBiasTee, biasTee_ ? 1 : 0);
        applyGain();

        quit_ = false;
        thread_ = std::thread([this, &sink] { run(sink); });
        return true;
    }

    void stop() override {
        quit_ = true;
        if (thread_.joinable()) thread_.join();
        connected_ = false;
        socket_.close();
    }

    void tune(double hz) override {
        center_ = hz;
        send(kSetFrequency, uint32_t(hz));
    }

    void drawSettings(bool running) override {
        ImGui::BeginDisabled(running);
        SharkSDR::ui::LabeledRow("Host");
        if (ImGui::InputText("##host", &hostName_)) save();
        SharkSDR::ui::LabeledRow("Port");
        if (ImGui::InputInt("##port", &port_, 0)) save();
        ImGui::EndDisabled();

        SharkSDR::ui::LabeledRow("Rate");
        if (ImGui::BeginCombo("##rate", SharkSDR::ui::formatSampleRate(rate_.load()).c_str())) {
            for (double r : kSampleRates) {
                if (ImGui::Selectable(SharkSDR::ui::formatSampleRate(r).c_str(), r == rate_.load())) {
                    rate_ = r;
                    send(kSetSampleRate, uint32_t(r));
                    save();
                }
            }
            ImGui::EndCombo();
        }

        const auto& gains = gainTable();
        gainIndex_ = std::clamp(gainIndex_, 0, int(gains.size()) - 1);
        if (ImGui::Checkbox("Tuner AGC", &tunerAgc_)) {
            applyGain();
            save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Automatic gain of the tuner chip. Turn it off to set the gain by hand\n"
                              "(usually gives a better signal-to-noise ratio).");
        SharkSDR::ui::SameLineColumn();
        if (ImGui::Checkbox("RTL AGC", &rtlAgc_)) {
            send(kSetAgc, rtlAgc_ ? 1 : 0);
            save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Digital AGC of the RTL2832. It usually lifts the noise floor:\n"
                              "leave it off for the best signal-to-noise ratio.");

        // The slider works in dB and snaps to the steps the tuner supports.
        ImGui::BeginDisabled(tunerAgc_);
        SharkSDR::ui::LabeledRow("Gain");
        float db = gains[size_t(gainIndex_)] / 10.0f;
        if (SharkSDR::ui::SliderFloat("##gain", &db, gains.front() / 10.0f, gains.back() / 10.0f,
                                      tunerAgc_ ? "Auto" : "%.1f dB")) {
            gainIndex_ = nearestGain(gains, db);
            applyGain();
            save();
        }
        ImGui::EndDisabled();

        SharkSDR::ui::LabeledRow("PPM");
        if (ImGui::InputInt("##ppm", &ppm_)) {
            send(kSetPpm, uint32_t(ppm_));
            save();
        }
        SharkSDR::ui::LabeledRow("Direct");
        const char* modes[] = {"Off", "I branch", "Q branch (HF)"};
        if (ImGui::Combo("##direct", &directSampling_, modes, 3)) {
            send(kSetDirectSampling, uint32_t(directSampling_));
            save();
        }
        if (ImGui::Checkbox("Bias-T", &biasTee_)) {
            send(kSetBiasTee, biasTee_ ? 1 : 0);
            save();
        }
        SharkSDR::ui::SameLineColumn();
        if (ImGui::Checkbox("Offset", &offsetTuning_)) {
            send(kSetOffsetTuning, offsetTuning_ ? 1 : 0);
            save();
        }
        if (connected_) ImGui::TextDisabled("Tuner: %s", tunerName(tunerType_));
        else ImGui::TextDisabled("Run: rtl_tcp -a 0.0.0.0 on the host");
    }

private:
    const std::vector<int>& gainTable() const {
        switch (tunerType_) {
            case 1: return kE4000Gains;
            case 2: return kFc0012Gains;
            case 3: return kFc0013Gains;
            case 4: return kFc2580Gains;
            default: return kR820TGains;  // R820T / R828D
        }
    }

    void applyGain() {
        // rtl_tcp gain mode: 0 = automatic (tuner AGC), 1 = manual.
        send(kSetGainMode, tunerAgc_ ? 0 : 1);
        if (!tunerAgc_) send(kSetGain, uint32_t(gainTable()[size_t(std::clamp(gainIndex_, 0, int(gainTable().size()) - 1))]));
    }

    void send(uint8_t cmd, uint32_t value) {
        if (!connected_) return;
        unsigned char buf[5] = {cmd, uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
        std::lock_guard<std::mutex> lock(sendMutex_);
        socket_.sendAll(buf, sizeof(buf));
    }

    void save() {
        auto& c = host_.config();
        c["host"] = hostName_;
        c["port"] = port_;
        c["sampleRate"] = rate_.load();
        c["tunerAgc"] = tunerAgc_;
        c["gainIndex"] = gainIndex_;
        c["ppm"] = ppm_;
        c["rtlAgc"] = rtlAgc_;
        c["directSampling"] = directSampling_;
        c["biasTee"] = biasTee_;
        c["offsetTuning"] = offsetTuning_;
    }

    void run(SharkSDR::SampleSink& sink) {
        std::vector<unsigned char> raw(1 << 16);
        std::vector<Complex> iq(raw.size() / 2 + 1);
        int leftover = -1;  // odd byte carried over between reads
        while (!quit_) {
            int n = socket_.receive(raw.data(), raw.size(), 100);
            if (n == 0) continue;
            if (n < 0) {
                if (!quit_) sink.sourceStopped("rtl_tcp connection closed");
                return;
            }
            size_t count = 0;
            size_t i = 0;
            if (leftover >= 0) {
                iq[count++] = Complex(lut_[size_t(leftover)], lut_[raw[0]]);
                i = 1;
                leftover = -1;
            }
            for (; i + 1 < size_t(n); i += 2) iq[count++] = Complex(lut_[raw[i]], lut_[raw[i + 1]]);
            if (i < size_t(n)) leftover = raw[i];
            sink.pushSamples(iq.data(), count);
        }
    }

    SharkSDR::Host& host_;
    SharkSDR::net::TcpSocket socket_;
    std::mutex sendMutex_;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> connected_{false};
    std::array<float, 256> lut_{};

    std::string hostName_ = "127.0.0.1";
    int port_ = 1234;
    std::atomic<double> rate_{2.4e6};
    std::atomic<double> center_{100e6};
    bool tunerAgc_ = true;
    int gainIndex_ = 14;
    int ppm_ = 0;
    bool rtlAgc_ = false;
    int directSampling_ = 0;
    bool biasTee_ = false;
    bool offsetTuning_ = false;
    uint32_t tunerType_ = 5;
    std::string error_;
};

class RtlTcpPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        source_ = std::make_shared<RtlTcpSource>(host);
        host.registerSource(source_);
    }
    void shutdown() override {
        if (source_) source_->stop();
        source_.reset();
    }

private:
    std::shared_ptr<RtlTcpSource> source_;
};

}  // namespace

SHARKSDR_PLUGIN(RtlTcpPlugin, "RTL-TCP Source", "1.0.0", "SharkSDR",
                "Network client for rtl_tcp servers (RTL-SDR dongles on any machine).")
