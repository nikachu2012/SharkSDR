// Native RTL-SDR source using librtlsdr (osmocom).

#include <SharkSDR/plugin.h>
#include <SharkSDR/ui/widgets.h>

#include <rtl-sdr.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using SharkSDR::Complex;

const double kSampleRates[] = {250e3, 1.024e6, 1.4e6, 1.8e6, 1.92e6, 2.048e6, 2.4e6, 2.56e6, 2.88e6, 3.2e6};

class RtlSdrSource : public SharkSDR::Source {
public:
    explicit RtlSdrSource(SharkSDR::Host& host) : host_(host) {
        for (int i = 0; i < 256; i++) lut_[size_t(i)] = (float(i) - 127.5f) / 127.5f;
        const auto& c = host_.config();
        wantedSerial_ = c.value("serial", std::string());
        rate_ = c.value("sampleRate", 2.4e6);
        // "manualGain" is the name used by older versions (inverse meaning).
        tunerAgc_ = c.contains("tunerAgc") ? c.value("tunerAgc", true) : !c.value("manualGain", false);
        gainIndex_ = c.value("gainIndex", 10);
        ppm_ = c.value("ppm", 0);
        rtlAgc_ = c.value("rtlAgc", false);
        directSampling_ = c.value("directSampling", 0);
        biasTee_ = c.value("biasTee", false);
        offsetTuning_ = c.value("offsetTuning", false);
    }
    ~RtlSdrSource() override { stop(); }

    std::string name() const override { return "RTL-SDR"; }
    double sampleRate() const override { return rate_.load(); }
    // The RTL2832U's decimation filter rolls off above about 80 % of the rate.
    double usableSpectrumRatio() const override { return 0.8; }
    std::string lastError() const override { return error_; }

    void onSelected() override {
        if (devices_.empty()) refresh();
    }

    bool start(SharkSDR::SampleSink& sink) override {
        stop();
        error_.clear();
        if (selected_ < 0 || selected_ >= int(devices_.size())) {
            refresh();
            if (selected_ < 0) {
                error_ = "No RTL-SDR device found";
                return false;
            }
        }
        if (rtlsdr_open(&dev_, uint32_t(selected_)) != 0 || !dev_) {
            error_ = "Cannot open RTL-SDR device (in use or missing driver?)";
            dev_ = nullptr;
            return false;
        }
        int count = rtlsdr_get_tuner_gains(dev_, nullptr);
        gains_.assign(size_t(std::max(count, 0)), 0);
        if (count > 0) rtlsdr_get_tuner_gains(dev_, gains_.data());

        rtlsdr_set_sample_rate(dev_, uint32_t(rate_.load()));
        rtlsdr_set_center_freq(dev_, uint32_t(center_.load()));
        if (ppm_ != 0) rtlsdr_set_freq_correction(dev_, ppm_);
        rtlsdr_set_direct_sampling(dev_, directSampling_);
        rtlsdr_set_offset_tuning(dev_, offsetTuning_ ? 1 : 0);
        rtlsdr_set_agc_mode(dev_, rtlAgc_ ? 1 : 0);
#if defined(SHARKSDR_RTLSDR_BIAS_TEE)
        rtlsdr_set_bias_tee(dev_, biasTee_ ? 1 : 0);
#endif
        applyGain();
        rtlsdr_reset_buffer(dev_);

        thread_ = std::thread([this, &sink] {
            sink_ = &sink;
            rtlsdr_read_async(dev_, &RtlSdrSource::callback, this, 0, 16384 * 4);
            if (!stopping_) sink.sourceStopped("RTL-SDR stream ended");
        });
        return true;
    }

    void stop() override {
        if (!dev_) return;
        stopping_ = true;
        rtlsdr_cancel_async(dev_);
        if (thread_.joinable()) thread_.join();
        rtlsdr_close(dev_);
        dev_ = nullptr;
        stopping_ = false;
    }

    void tune(double hz) override {
        center_ = hz;
        std::lock_guard<std::mutex> lock(mutex_);
        if (dev_) rtlsdr_set_center_freq(dev_, uint32_t(hz));
    }

    void drawSettings(bool running) override {
        ImGui::BeginDisabled(running);
        SharkSDR::ui::LabeledRow("Device");
        const float refreshW = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - refreshW - ImGui::GetStyle().ItemSpacing.x);
        const char* cur = selected_ >= 0 && selected_ < int(devices_.size()) ? devices_[size_t(selected_)].c_str() : "(none)";
        if (ImGui::BeginCombo("##dev", cur)) {
            for (int i = 0; i < int(devices_.size()); i++) {
                if (ImGui::Selectable(devices_[size_t(i)].c_str(), i == selected_)) {
                    selected_ = i;
                    wantedSerial_ = serials_[size_t(i)];
                    save();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) refresh();
        ImGui::EndDisabled();

        SharkSDR::ui::LabeledRow("Rate");
        if (ImGui::BeginCombo("##rate", SharkSDR::ui::formatSampleRate(rate_.load()).c_str())) {
            for (double r : kSampleRates) {
                if (ImGui::Selectable(SharkSDR::ui::formatSampleRate(r).c_str(), r == rate_.load())) {
                    rate_ = r;
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (dev_) rtlsdr_set_sample_rate(dev_, uint32_t(r));
                    save();
                }
            }
            ImGui::EndCombo();
        }

        if (ImGui::Checkbox("Tuner AGC", &tunerAgc_)) {
            applyGain();
            save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Automatic gain of the tuner chip. Turn it off to set the gain by hand\n"
                              "(usually gives a better signal-to-noise ratio).");
        SharkSDR::ui::SameLineColumn();
        if (ImGui::Checkbox("RTL AGC", &rtlAgc_)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (dev_) rtlsdr_set_agc_mode(dev_, rtlAgc_ ? 1 : 0);
            save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Digital AGC of the RTL2832. It usually lifts the noise floor:\n"
                              "leave it off for the best signal-to-noise ratio.");

        // The slider works in dB and snaps to the steps the tuner supports.
        ImGui::BeginDisabled(tunerAgc_ || gains_.empty());
        SharkSDR::ui::LabeledRow("Gain");
        gainIndex_ = std::clamp(gainIndex_, 0, std::max(0, int(gains_.size()) - 1));
        float db = gains_.empty() ? 0.0f : gains_[size_t(gainIndex_)] / 10.0f;
        const char* format = tunerAgc_ ? "Auto" : gains_.empty() ? "start to read gains" : "%.1f dB";
        if (SharkSDR::ui::SliderFloat("##gain", &db, gains_.empty() ? 0.0f : gains_.front() / 10.0f,
                                      gains_.empty() ? 0.0f : gains_.back() / 10.0f, format) &&
            !gains_.empty()) {
            gainIndex_ = nearestGain(db);
            applyGain();
            save();
        }
        ImGui::EndDisabled();

        SharkSDR::ui::LabeledRow("PPM");
        if (ImGui::InputInt("##ppm", &ppm_)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (dev_) rtlsdr_set_freq_correction(dev_, ppm_);
            save();
        }
        SharkSDR::ui::LabeledRow("Direct");
        const char* modes[] = {"Off", "I branch", "Q branch (HF)"};
        if (ImGui::Combo("##direct", &directSampling_, modes, 3)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (dev_) rtlsdr_set_direct_sampling(dev_, directSampling_);
            save();
        }
#if defined(SHARKSDR_RTLSDR_BIAS_TEE)
        if (ImGui::Checkbox("Bias-T", &biasTee_)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (dev_) rtlsdr_set_bias_tee(dev_, biasTee_ ? 1 : 0);
            save();
        }
        SharkSDR::ui::SameLineColumn();
#endif
        if (ImGui::Checkbox("Offset", &offsetTuning_)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (dev_) rtlsdr_set_offset_tuning(dev_, offsetTuning_ ? 1 : 0);
            save();
        }
    }

private:
    static void callback(unsigned char* buf, uint32_t len, void* ctx) {
        auto* self = static_cast<RtlSdrSource*>(ctx);
        if (self->stopping_) {
            // stop() may have called rtlsdr_cancel_async() before read_async()
            // started (which then has no effect): cancel again from here.
            rtlsdr_cancel_async(self->dev_);
            return;
        }
        if (!self->sink_) return;
        self->iq_.resize(len / 2);
        for (uint32_t i = 0; i + 1 < len; i += 2) self->iq_[i / 2] = Complex(self->lut_[buf[i]], self->lut_[buf[i + 1]]);
        self->sink_->pushSamples(self->iq_.data(), self->iq_.size());
    }

    void refresh() {
        devices_.clear();
        serials_.clear();
        const uint32_t n = rtlsdr_get_device_count();
        selected_ = -1;
        for (uint32_t i = 0; i < n; i++) {
            char vendor[256] = {}, product[256] = {}, serial[256] = {};
            rtlsdr_get_device_usb_strings(i, vendor, product, serial);
            devices_.push_back(std::string(rtlsdr_get_device_name(i)) + " (" + serial + ")");
            serials_.push_back(serial);
            if (serial == wantedSerial_) selected_ = int(i);
        }
        if (selected_ < 0 && n > 0) selected_ = 0;
    }

    // Index of the gain step closest to `db`.
    int nearestGain(float db) const {
        int best = 0;
        for (int i = 1; i < int(gains_.size()); i++)
            if (std::fabs(gains_[size_t(i)] - db * 10.0f) < std::fabs(gains_[size_t(best)] - db * 10.0f)) best = i;
        return best;
    }

    void applyGain() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dev_) return;
        rtlsdr_set_tuner_gain_mode(dev_, tunerAgc_ ? 0 : 1);  // 0 = automatic, 1 = manual
        if (!tunerAgc_ && !gains_.empty())
            rtlsdr_set_tuner_gain(dev_, gains_[size_t(std::clamp(gainIndex_, 0, int(gains_.size()) - 1))]);
    }

    void save() {
        auto& c = host_.config();
        c["serial"] = wantedSerial_;
        c["sampleRate"] = rate_.load();
        c["tunerAgc"] = tunerAgc_;
        c["gainIndex"] = gainIndex_;
        c["ppm"] = ppm_;
        c["rtlAgc"] = rtlAgc_;
        c["directSampling"] = directSampling_;
        c["biasTee"] = biasTee_;
        c["offsetTuning"] = offsetTuning_;
    }

    SharkSDR::Host& host_;
    rtlsdr_dev_t* dev_ = nullptr;
    std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    SharkSDR::SampleSink* sink_ = nullptr;
    std::vector<Complex> iq_;
    std::array<float, 256> lut_{};

    std::vector<std::string> devices_, serials_;
    int selected_ = -1;
    std::string wantedSerial_;
    std::vector<int> gains_;
    std::atomic<double> rate_{2.4e6};
    std::atomic<double> center_{100e6};
    bool tunerAgc_ = true;
    int gainIndex_ = 10;
    int ppm_ = 0;
    bool rtlAgc_ = false;
    int directSampling_ = 0;
    bool biasTee_ = false;
    bool offsetTuning_ = false;
    std::string error_;
};

class RtlSdrPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        source_ = std::make_shared<RtlSdrSource>(host);
        host.registerSource(source_);
    }
    void shutdown() override { source_.reset(); }

private:
    std::shared_ptr<RtlSdrSource> source_;
};

}  // namespace

SHARKSDR_PLUGIN(RtlSdrPlugin, "RTL-SDR Source", "1.0.0", "SharkSDR", "Native RTL-SDR support through librtlsdr.")
