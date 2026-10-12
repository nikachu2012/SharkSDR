// SoapySDR source: supports every device that has a SoapySDR module
// (RTL-SDR, HackRF, Airspy, SDRplay, LimeSDR, PlutoSDR, USRP, ...).

#include <SharkSDR/plugin.h>
#include <SharkSDR/ui/widgets.h>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Version.hpp>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using SharkSDR::Complex;

std::string deviceLabel(const SoapySDR::Kwargs& args) {
    auto it = args.find("label");
    if (it != args.end()) return it->second;
    std::string s = args.count("driver") ? args.at("driver") : "device";
    if (args.count("serial")) s += " " + args.at("serial");
    return s;
}

class SoapySource : public SharkSDR::Source {
public:
    explicit SoapySource(SharkSDR::Host& host) : host_(host) {
        const auto& c = host_.config();
        wantedDevice_ = c.value("device", std::string());
        rate_ = c.value("sampleRate", 2.4e6);
        agc_ = c.value("agc", false);
        ppm_ = c.value("ppm", 0.0);
        if (c.contains("gains") && c["gains"].is_object())
            for (auto& [k, v] : c["gains"].items()) gains_[k] = v.get<double>();
        antenna_ = c.value("antenna", std::string());
    }

    ~SoapySource() override {
        stop();
        closeDevice();
    }

    std::string name() const override { return "SoapySDR"; }
    double sampleRate() const override { return rate_.load(); }
    std::string lastError() const override { return error_; }

    void onSelected() override {
        if (devices_.empty()) refresh();
    }

    bool start(SharkSDR::SampleSink& sink) override {
        stop();
        error_.clear();
        if (!device_ && !openDevice()) return false;
        try {
            device_->setSampleRate(SOAPY_SDR_RX, 0, rate_.load());
            device_->setFrequency(SOAPY_SDR_RX, 0, center_.load());
            applySettings();
            stream_ = device_->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
            if (device_->activateStream(stream_) != 0) throw std::runtime_error("activateStream failed");
        } catch (const std::exception& e) {
            error_ = std::string("SoapySDR: ") + e.what();
            if (stream_) device_->closeStream(stream_);
            stream_ = nullptr;
            return false;
        }
        quit_ = false;
        thread_ = std::thread([this, &sink] { run(sink); });
        return true;
    }

    void stop() override {
        quit_ = true;
        if (thread_.joinable()) thread_.join();
        if (device_ && stream_) {
            device_->deactivateStream(stream_);
            device_->closeStream(stream_);
        }
        stream_ = nullptr;
    }

    void tune(double hz) override {
        center_ = hz;
        std::lock_guard<std::mutex> lock(deviceMutex_);
        if (device_) {
            try {
                device_->setFrequency(SOAPY_SDR_RX, 0, hz);
            } catch (const std::exception& e) {
                host_.log(SharkSDR::LogLevel::Warning, std::string("setFrequency: ") + e.what());
            }
        }
    }

    void drawSettings(bool running) override {
        ImGui::BeginDisabled(running);
        SharkSDR::ui::LabeledRow("Device");
        const float refreshW = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - refreshW - ImGui::GetStyle().ItemSpacing.x);
        const std::string current = selected_ >= 0 && selected_ < int(devices_.size()) ? deviceLabel(devices_[size_t(selected_)]) : "(none)";
        if (ImGui::BeginCombo("##dev", current.c_str())) {
            for (int i = 0; i < int(devices_.size()); i++) {
                if (ImGui::Selectable(deviceLabel(devices_[size_t(i)]).c_str(), i == selected_)) {
                    closeDevice();
                    selected_ = i;
                    wantedDevice_ = deviceLabel(devices_[size_t(i)]);
                    openDevice();
                    save();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) refresh();
        ImGui::EndDisabled();

        if (!device_) {
            ImGui::TextDisabled(devices_.empty() ? "No SoapySDR device found" : "Select a device");
            if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error_.c_str());
            return;
        }

        SharkSDR::ui::LabeledRow("Rate");
        if (ImGui::BeginCombo("##rate", SharkSDR::ui::formatSampleRate(rate_.load()).c_str())) {
            for (double r : rates_) {
                if (ImGui::Selectable(SharkSDR::ui::formatSampleRate(r).c_str(), r == rate_.load())) {
                    rate_ = r;
                    std::lock_guard<std::mutex> lock(deviceMutex_);
                    try {
                        device_->setSampleRate(SOAPY_SDR_RX, 0, r);
                    } catch (...) {
                    }
                    save();
                }
            }
            ImGui::EndCombo();
        }

        if (antennas_.size() > 1) {
            SharkSDR::ui::LabeledRow("Antenna");
            if (ImGui::BeginCombo("##ant", antenna_.c_str())) {
                for (const auto& a : antennas_) {
                    if (ImGui::Selectable(a.c_str(), a == antenna_)) {
                        antenna_ = a;
                        applySettings();
                        save();
                    }
                }
                ImGui::EndCombo();
            }
        }

        if (hasAgc_ && ImGui::Checkbox("Automatic gain (AGC)", &agc_)) {
            applySettings();
            save();
        }
        ImGui::BeginDisabled(hasAgc_ && agc_);
        for (const auto& [gname, range] : gainRanges_) {
            float v = float(gains_.count(gname) ? gains_[gname] : range.minimum());
            SharkSDR::ui::LabeledRow(gname.c_str());
            if (SharkSDR::ui::SliderFloat(("##g_" + gname).c_str(), &v, float(range.minimum()), float(range.maximum()), "%.1f dB")) {
                gains_[gname] = v;
                std::lock_guard<std::mutex> lock(deviceMutex_);
                try {
                    device_->setGain(SOAPY_SDR_RX, 0, gname, v);
                } catch (...) {
                }
                save();
            }
        }
        ImGui::EndDisabled();

        if (hasPpm_) {
            float ppm = float(ppm_);
            SharkSDR::ui::LabeledRow("PPM");
            if (ImGui::DragFloat("##ppm", &ppm, 0.1f, -200.0f, 200.0f, "%.1f")) {
                ppm_ = ppm;
                applySettings();
                save();
            }
        }
        if (!error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error_.c_str());
    }

private:
    void refresh() {
        try {
            devices_ = SoapySDR::Device::enumerate();
        } catch (const std::exception& e) {
            error_ = e.what();
            devices_.clear();
        }
        selected_ = -1;
        for (int i = 0; i < int(devices_.size()); i++)
            if (deviceLabel(devices_[size_t(i)]) == wantedDevice_) selected_ = i;
        if (selected_ < 0 && !devices_.empty()) selected_ = 0;
        host_.log(SharkSDR::LogLevel::Info, "Found " + std::to_string(devices_.size()) + " SoapySDR device(s)");
    }

    bool openDevice() {
        if (selected_ < 0 || selected_ >= int(devices_.size())) {
            error_ = "No device selected";
            return false;
        }
        std::lock_guard<std::mutex> lock(deviceMutex_);
        try {
            device_ = SoapySDR::Device::make(devices_[size_t(selected_)]);
        } catch (const std::exception& e) {
            error_ = std::string("Cannot open device: ") + e.what();
            device_ = nullptr;
            return false;
        }
        if (!device_) {
            error_ = "Cannot open device";
            return false;
        }
        // Capabilities.
        rates_ = device_->listSampleRates(SOAPY_SDR_RX, 0);
        if (rates_.empty()) {
            for (const auto& r : device_->getSampleRateRange(SOAPY_SDR_RX, 0))
                for (double v : {250e3, 1.024e6, 2.048e6, 2.4e6, 3.2e6, 5e6, 6e6, 8e6, 10e6, 20e6})
                    if (v >= r.minimum() && v <= r.maximum()) rates_.push_back(v);
            std::sort(rates_.begin(), rates_.end());
            rates_.erase(std::unique(rates_.begin(), rates_.end()), rates_.end());
        }
        if (!rates_.empty() && std::find(rates_.begin(), rates_.end(), rate_.load()) == rates_.end()) {
            double best = rates_.front();
            for (double r : rates_)
                if (std::abs(r - 2.4e6) < std::abs(best - 2.4e6)) best = r;
            rate_ = best;
        }
        antennas_ = device_->listAntennas(SOAPY_SDR_RX, 0);
        if (antenna_.empty() || std::find(antennas_.begin(), antennas_.end(), antenna_) == antennas_.end())
            antenna_ = antennas_.empty() ? "" : device_->getAntenna(SOAPY_SDR_RX, 0);
        hasAgc_ = device_->hasGainMode(SOAPY_SDR_RX, 0);
        hasPpm_ = device_->hasFrequencyCorrection(SOAPY_SDR_RX, 0);
        gainRanges_.clear();
        for (const auto& g : device_->listGains(SOAPY_SDR_RX, 0)) gainRanges_[g] = device_->getGainRange(SOAPY_SDR_RX, 0, g);
        error_.clear();
        host_.log(SharkSDR::LogLevel::Info, "Opened " + deviceLabel(devices_[size_t(selected_)]));
        return true;
    }

    void closeDevice() {
        stop();
        std::lock_guard<std::mutex> lock(deviceMutex_);
        if (device_) SoapySDR::Device::unmake(device_);
        device_ = nullptr;
    }

    void applySettings() {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        if (!device_) return;
        try {
            if (!antenna_.empty()) device_->setAntenna(SOAPY_SDR_RX, 0, antenna_);
            if (hasAgc_) device_->setGainMode(SOAPY_SDR_RX, 0, agc_);
            if (!agc_ || !hasAgc_)
                for (const auto& [g, v] : gains_)
                    if (gainRanges_.count(g)) device_->setGain(SOAPY_SDR_RX, 0, g, v);
            if (hasPpm_) device_->setFrequencyCorrection(SOAPY_SDR_RX, 0, ppm_);
        } catch (const std::exception& e) {
            host_.log(SharkSDR::LogLevel::Warning, e.what());
        }
    }

    void save() {
        auto& c = host_.config();
        c["device"] = wantedDevice_;
        c["sampleRate"] = rate_.load();
        c["agc"] = agc_;
        c["ppm"] = ppm_;
        c["antenna"] = antenna_;
        c["gains"] = gains_;
    }

    void run(SharkSDR::SampleSink& sink) {
        const size_t mtu = std::max<size_t>(1024, device_->getStreamMTU(stream_));
        std::vector<Complex> buffer(mtu);
        int errors = 0;
        while (!quit_) {
            void* buffs[] = {buffer.data()};
            int flags = 0;
            long long timeNs = 0;
            const int n = device_->readStream(stream_, buffs, mtu, flags, timeNs, 200000);
            if (n > 0) {
                sink.pushSamples(buffer.data(), size_t(n));
                errors = 0;
            } else if (n == SOAPY_SDR_TIMEOUT || n == SOAPY_SDR_OVERFLOW) {
                continue;
            } else if (++errors > 50) {
                sink.sourceStopped("SoapySDR stream error " + std::to_string(n));
                return;
            }
        }
    }

    SharkSDR::Host& host_;
    SoapySDR::KwargsList devices_;
    int selected_ = -1;
    std::string wantedDevice_;
    SoapySDR::Device* device_ = nullptr;
    SoapySDR::Stream* stream_ = nullptr;
    std::mutex deviceMutex_;

    std::vector<double> rates_;
    std::vector<std::string> antennas_;
    std::map<std::string, SoapySDR::Range> gainRanges_;
    std::map<std::string, double> gains_;
    std::string antenna_;
    bool hasAgc_ = false;
    bool hasPpm_ = false;
    bool agc_ = false;
    double ppm_ = 0.0;

    std::atomic<double> rate_{2.4e6};
    std::atomic<double> center_{100e6};
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::string error_;
};

class SoapyPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        source_ = std::make_shared<SoapySource>(host);
        host.registerSource(source_);
    }
    void shutdown() override { source_.reset(); }

private:
    std::shared_ptr<SoapySource> source_;
};

}  // namespace

SHARKSDR_PLUGIN(SoapyPlugin, "SoapySDR Source", "1.0.0", "SharkSDR",
                "Hardware support through SoapySDR (RTL-SDR, HackRF, Airspy, SDRplay, LimeSDR, PlutoSDR, ...).")
