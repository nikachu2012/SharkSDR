// Example of a demodulator plugin: synchronous AM ("SAM").
//
// A PLL locks onto the AM carrier and the signal is coherently detected,
// which reduces selective-fading distortion compared to an envelope detector.
// Optionally only one sideband is used (useful with an interferer on the
// other side). All the channel filtering is done by the application; the
// plugin only implements the demodulation itself.

#include <SharkSDR/dsp/filter.h>
#include <SharkSDR/plugin.h>
#include <SharkSDR/ui/widgets.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace {

using SharkSDR::Complex;

class SyncAmDemodulator : public SharkSDR::Demodulator {
public:
    SyncAmDemodulator() {
        carrier_.configureCutoff(5.0, kRate);
        lock_.configureCutoff(2.0, kRate);
    }

    double channelRate() const override { return kRate; }
    double defaultBandwidth() const override { return 9000.0; }
    double minBandwidth() const override { return 2000.0; }
    double maxBandwidth() const override { return 20000.0; }
    double defaultSnap() const override { return 1000.0; }
    double audioCutoff() const override { return std::max(1000.0, bandwidth_ * 0.5); }
    void setBandwidth(double hz) override { bandwidth_ = hz; }

    void reset() override {
        phase_ = 0.0;
        freq_ = 0.0;
        carrier_.reset();
        lock_.reset();
    }

    void process(const Complex* in, size_t count, SharkSDR::Stereo* out) override {
        // Loop bandwidth ~30 Hz.
        const double wn = SharkSDR::kTwoPi * 30.0 / kRate;
        const double kp = 2.0 * 0.707 * wn, ki = wn * wn;
        const double maxPull = SharkSDR::kTwoPi * 600.0 / kRate;
        for (size_t i = 0; i < count; i++) {
            const Complex lo(float(std::cos(phase_)), float(-std::sin(phase_)));
            const Complex y = SharkSDR::cmul(in[i], lo);  // carrier now at 0 Hz, in phase with I
            const double err = std::atan2(double(y.imag()), double(y.real()) + 1e-12);
            freq_ = std::clamp(freq_ + ki * err, -maxPull, maxPull);
            phase_ += freq_ + kp * err;
            if (phase_ > SharkSDR::kTwoPi) phase_ -= SharkSDR::kTwoPi;
            if (phase_ < 0.0) phase_ += SharkSDR::kTwoPi;
            lock_.process(float(std::cos(err)));

            // I carries carrier + both sidebands, Q only their difference:
            // I - H{Q} keeps the upper sideband, I + H{Q} the lower one.
            float iDelayed = 0.0f;
            const float hq = hilbert(y.real(), y.imag(), &iDelayed);
            float v = y.real();
            if (sideband_ == 1) v = iDelayed - hq;
            if (sideband_ == 2) v = iDelayed + hq;
            const float c = std::max(carrier_.process(std::fabs(y.real())), 1e-9f);
            out[i] = SharkSDR::Stereo(std::clamp((v / c - 1.0f) * 0.8f, -1.0f, 1.0f));
        }
    }

    void drawSettings() override {
        SharkSDR::ui::LabeledRow("Sideband");
        const char* items[] = {"Both (DSB)", "Upper", "Lower"};
        ImGui::Combo("##sb", &sideband_, items, 3);
        ImGui::TextDisabled("Carrier PLL: %s", locked() ? "locked" : "searching");
        ImGui::TextDisabled("Offset: %+.1f Hz", freq_ * kRate / SharkSDR::kTwoPi);
    }

    std::string status() const override { return locked() ? "Locked" : "Unlocked"; }

    void loadState(const nlohmann::json& j) override { sideband_ = std::clamp(j.value("sideband", 0), 0, 2); }
    void saveState(nlohmann::json& j) const override { j["sideband"] = sideband_; }

private:
    static constexpr double kRate = 24000.0;

    bool locked() const { return lock_.value() > 0.8f; }

    // FIR Hilbert transformer (2*kHalf+1 taps) applied to q. Returns H{q} and
    // the matching kHalf-sample delayed i, both aligned to time t - kHalf.
    float hilbert(float i, float q, float* iDelayed) {
        histI_[pos_] = i;
        histQ_[pos_] = q;
        auto at = [&](const float* h, int delay) { return h[(pos_ - delay + 2 * kLen) % kLen]; };
        float acc = 0.0f;
        for (int k = 1; k <= kHalf; k += 2) {  // the even taps are zero
            const float w = float(0.54 + 0.46 * std::cos(SharkSDR::kPi * k / (kHalf + 1)));
            const float h = float(2.0 / (SharkSDR::kPi * k)) * w;
            acc += h * (at(histQ_, kHalf + k) - at(histQ_, kHalf - k));
        }
        *iDelayed = at(histI_, kHalf);
        pos_ = (pos_ + 1) % kLen;
        return acc;
    }

    static constexpr int kHalf = 31;
    static constexpr int kLen = 2 * kHalf + 1;

    double bandwidth_ = 9000.0;
    int sideband_ = 0;
    double phase_ = 0.0;
    double freq_ = 0.0;
    SharkSDR::dsp::OnePole carrier_;
    SharkSDR::dsp::OnePole lock_;
    float histI_[kLen] = {};
    float histQ_[kLen] = {};
    int pos_ = 0;
};

class AmSyncPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        host.registerDemodulator("SAM", [] { return std::make_unique<SyncAmDemodulator>(); });
    }
};

}  // namespace

SHARKSDR_PLUGIN(AmSyncPlugin, "Synchronous AM", "1.0.0", "SharkSDR",
                "Adds the SAM mode: PLL based synchronous AM detector with sideband selection.")
