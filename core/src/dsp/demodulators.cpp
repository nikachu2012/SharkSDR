#include <SharkSDR/dsp/agc.h>
#include <SharkSDR/dsp/demodulators.h>
#include <SharkSDR/dsp/filter.h>
#include <SharkSDR/dsp/math.h>
#include <SharkSDR/dsp/nco.h>
#include <SharkSDR/dsp/noise_reduction.h>
#include <SharkSDR/ui/widgets.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace SharkSDR::dsp {
namespace {

// De-emphasis time constants selectable in the UI.
const char* const kDeemphasisNames[] = {"None", "50 us", "75 us"};
const double kDeemphasisTau[] = {0.0, 50e-6, 75e-6};

bool deemphasisCombo(const char* label, int* index) {
    ui::LabeledRow("De-emphasis");
    return ImGui::Combo(label, index, kDeemphasisNames, IM_ARRAYSIZE(kDeemphasisNames));
}

// IF noise reduction for the FM modes (see FmIfNoiseReduction).
struct IfnrPreset {
    size_t fftSize;
    size_t keepBins;
};

bool ifnrCombo(const char* label, int* level) {
    static const char* const names[] = {"Off", "Normal", "Strong"};
    ui::LabeledRow("IF NR");
    const bool changed = ImGui::Combo(label, level, names, IM_ARRAYSIZE(names));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("IF noise reduction: follows the FM carrier and removes the noise around it.\n"
                          "Helps a lot on weak stations (fewer clicks and less hiss);\n"
                          "\"Strong\" can dull high frequencies on strong signals.");
    return changed;
}

// Runs the noise reduction when enabled; returns the samples to demodulate.
const Complex* applyIfnr(int level, FmIfNoiseReduction& nr, std::vector<Complex>& buffer, const Complex* in, size_t count) {
    if (level <= 0) return in;
    buffer.resize(count);
    nr.process(in, count, buffer.data());
    return buffer.data();
}

// ---- Wide band FM (broadcast), with stereo decoder ----------------------------

class WfmDemodulator : public Demodulator {
public:
    WfmDemodulator() {
        upsampler_.configure(100000.0, 2.0 * kRate, 80.0);
        downsampler_.configure(60000.0, 2.0 * kRate, 80.0);
        configure();
    }

    double channelRate() const override { return kRate; }
    double defaultBandwidth() const override { return 150000.0; }
    double minBandwidth() const override { return 50000.0; }
    double maxBandwidth() const override { return 250000.0; }
    double defaultSnap() const override { return 100000.0; }
    double audioCutoff() const override { return 15000.0; }

    void reset() override {
        prev_ = Complex(1.0f, 0.0f);
        pllPhase_ = 0.0;
        pllFreq_ = kPilotW;
        lockI_ = lockQ_ = 0.0f;
        for (float& stage : quad_) stage = 0.0f;
        noiseSum_ = 0.0f;
        noiseCount_ = 0;
        noiseBlocks_ = 0;
        noiseDb_ = 0.0f;
        blend_ = 0.0f;
        deemphL_.reset();
        deemphR_.reset();
        nr_.reset();
        upsampler_.reset();
        downsampler_.reset();
    }

    void process(const Complex* input, size_t count, Stereo* out) override {
        const Complex* in = applyIfnr(ifnr_, nr_, nrBuffer_, input, count);

        // Discriminate at twice the channel rate. Near the FM threshold the
        // phase steps between samples get large, and at the channel rate
        // many of them would wrap the wrong way round (heard as clicks):
        // measured +4..6 dB SINAD at CNR 6-8 dB.
        fast_.resize(2 * count);
        upsampler_.process(in, count, fast_.data());
        const float scale = float(2.0 * kRate / (kTwoPi * kDeviation));
        for (Complex& v : fast_) {
            const Complex d = cmulConj(v, prev_);
            prev_ = v;
            v = Complex(accurateAtan2(d.imag(), d.real()) * scale, 0.0f);
        }
        downsampler_.process(fast_.data(), 2 * count, fast_.data());  // back to `count` samples
        mpx_.resize(count);
        for (size_t i = 0; i < count; i++) mpx_[i] = fast_[i].real();
        processMpx(mpx_.data(), count, kRate);

        for (size_t i = 0; i < count; i++) {
            float mpx = mpx_[i];

            float l = mpx, r = mpx;
            if (stereoMode_ != kMono) {
                // Second order PLL locked to the 19 kHz pilot (pilot = cos(phase)).
                const float s = float(std::sin(pllPhase_));
                const float c = float(std::cos(pllPhase_));

                // Phase detector: the quadrature product, low-pass filtered
                // (4th order, ~400 Hz) so that programme audio mixed down from
                // 18 kHz and below is rejected by >100 dB and cannot jitter the
                // loop (that jitter would cost stereo separation).
                float q = mpx * s;
                for (float& stage : quad_) q = stage += kQuadAlpha * (q - stage);
                lockI_ += kLockAlpha * (mpx * c - lockI_);
                lockQ_ += kLockAlpha * (q - lockQ_);

                // Normalised by the pilot amplitude: err ~ sin(theta - phase).
                const double err = -double(q) / std::max(double(lockI_), 0.02);
                pllFreq_ = std::clamp(pllFreq_ + kKi * err, kPilotW - kMaxPull, kPilotW + kMaxPull);
                pllPhase_ += pllFreq_ + kKp * err;
                if (pllPhase_ > kTwoPi) pllPhase_ -= kTwoPi;

                // The quadrature signal carries no programme either, so its
                // power measures the noise floor around the pilot.
                // Averaged in dB (5 ms blocks) so that it settles quickly after
                // tuning instead of decaying exponentially over many decades.
                noiseSum_ += q * q;
                if (++noiseCount_ == kNoiseBlock) {
                    const float db = 10.0f * std::log10(noiseSum_ / float(kNoiseBlock) + 1e-20f);
                    // Converge quickly right after tuning, then average slowly.
                    noiseDb_ += (noiseBlocks_ < 60 ? 0.1f : kNoiseDbAlpha) * (db - noiseDb_);
                    if (noiseBlocks_ < 60) noiseBlocks_++;
                    noiseSum_ = 0.0f;
                    noiseCount_ = 0;
                }

                // Stereo costs ~20 dB of SNR: blend towards mono when the
                // signal is weak, like a hardware FM receiver.
                float target = 0.0f;
                if (locked()) target = stereoMode_ == kForcedStereo ? 1.0f : std::clamp((pilotSnrDb() - kBlendLowDb) / (kBlendHighDb - kBlendLowDb), 0.0f, 1.0f);
                // Slow to widen, quick to fall back to mono: avoids an audible
                // "breathing" of the stereo image on fluctuating signals.
                blend_ += (target > blend_ ? kBlendRise : kBlendFall) * (target - blend_);

                if (blend_ > 0.001f) {
                    // pilot = cos(phase) => subcarrier sin(2*theta) = -sin(2*phase).
                    // kSubcarrierGain undoes the discriminator droop at 38 kHz.
                    float diff = blend_ * kSubcarrierGain * mpx * (-4.0f * s * c);  // 2 * -sin(2*phase)
                    l = mpx + diff;
                    r = mpx - diff;
                }
            }
            if (deemphasis_ > 0) {
                l = deemphL_.process(l);
                r = deemphR_.process(r);
            }
            out[i] = Stereo(l, r);
        }
    }

    void drawSettings() override {
        static const char* modes[] = {"Mono", "Stereo (auto blend)", "Stereo (forced)"};
        ui::LabeledRow("Stereo");
        ImGui::Combo("##wfm_stereo", &stereoMode_, modes, IM_ARRAYSIZE(modes));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stereo reception is ~20 dB noisier than mono.\n"
                              "Auto blend fades to mono when the pilot signal-to-noise ratio is low.");
        if (stereoMode_ != kMono) {
            ui::LabeledRow("");
            if (locked()) {
                char label[48];
                std::snprintf(label, sizeof(label), "%.0f %%  SNR %.0f dB", blend_ * 100.0f, pilotSnrDb());
                ImGui::ProgressBar(blend_, ImVec2(-FLT_MIN, 0), label);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Stereo blend (0 %% = mono, 100 %% = full stereo)\nand signal-to-noise ratio of the 19 kHz pilot");
            } else {
                ImGui::TextDisabled("No stereo pilot");
            }
        }
        if (deemphasisCombo("##wfm_deemph", &deemphasis_)) configure();
        if (ifnrCombo("##wfm_ifnr", &ifnr_)) configure();
    }

    std::string status() const override {
        if (stereoMode_ == kMono || !locked() || blend_ < 0.05f) return "Mono";
        if (blend_ > 0.95f) return "Stereo";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Stereo %.0f%%", blend_ * 100.0f);
        return buf;
    }

    void loadState(const nlohmann::json& j) override {
        if (j.contains("stereoMode")) stereoMode_ = std::clamp(j.value("stereoMode", stereoMode_), 0, 2);
        else if (j.contains("stereo")) stereoMode_ = j.value("stereo", true) ? kAutoStereo : kMono;  // older settings
        deemphasis_ = std::clamp(j.value("deemphasis", deemphasis_), 0, 2);
        ifnr_ = std::clamp(j.value("ifnr", ifnr_), 0, 2);
        configure();
    }
    void saveState(nlohmann::json& j) const override {
        j["stereoMode"] = stereoMode_;
        j["deemphasis"] = deemphasis_;
        j["ifnr"] = ifnr_;
    }

private:
    static constexpr double kRate = 250000.0;
    static constexpr double kDeviation = 75000.0;
    static constexpr double kPilotW = kTwoPi * 19000.0 / kRate;
    static constexpr double kMaxPull = kTwoPi * 50.0 / kRate;
    // Natural frequency 10 Hz, damping 0.707 (phase detector gain is 1 after normalisation).
    static constexpr double kKp = 2.0 * 0.707 * (kTwoPi * 10.0 / kRate);
    static constexpr double kKi = (kTwoPi * 10.0 / kRate) * (kTwoPi * 10.0 / kRate);
    static constexpr float kLockAlpha = 2e-5f;
    static constexpr float kQuadAlpha = 0.01f;  // ~400 Hz per stage
    static constexpr float kBlendRise = 5.7e-6f;   // ~0.7 s towards stereo
    static constexpr float kBlendFall = 2.0e-5f;   // ~0.2 s towards mono
    static constexpr int kNoiseBlock = 1250;       // 5 ms
    static constexpr float kNoiseDbAlpha = 0.01f;  // ~0.5 s
    // IF noise reduction presets, chosen by measuring SINAD with a 150 kHz
    // channel: "Normal" gains ~9 dB at CNR 6-8 dB without hurting strong signals.
    static constexpr IfnrPreset kIfnrPresets[3] = {{0, 0}, {32, 10}, {32, 8}};
    // Pilot SNR range over which the receiver fades from mono to full stereo.
    // Measured: pilot SNR ~ CNR(200 kHz) + 18.5 dB, stereo audio SNR ~ pilot SNR - 5 dB.
    // With this range the audio SNR stays above ~55 dB until the receiver is fully mono.
    static constexpr float kBlendLowDb = 48.0f;
    static constexpr float kBlendHighDb = 63.0f;
    // The phase-difference discriminator has a sinc(f/fs) response (fs = the
    // doubled rate); at the 38 kHz subcarrier that is -0.08 dB, which would
    // limit separation to ~47 dB.
    static constexpr float kSubcarrierGain = 1.0096f;

    enum StereoMode { kMono = 0, kAutoStereo = 1, kForcedStereo = 2 };

    bool locked() const { return lockI_ > 0.015f && lockI_ > 3.0f * std::fabs(lockQ_); }
    float pilotSnrDb() const { return 20.0f * std::log10(std::max(lockI_, 1e-6f)) - noiseDb_; }

    void configure() {
        deemphL_.configureTau(kDeemphasisTau[deemphasis_], kRate);
        deemphR_.configureTau(kDeemphasisTau[deemphasis_], kRate);
        if (ifnr_ > 0) nr_.configure(kIfnrPresets[ifnr_].fftSize, kIfnrPresets[ifnr_].keepBins);
    }

    int stereoMode_ = kAutoStereo;
    int ifnr_ = 0;
    FmIfNoiseReduction nr_;
    std::vector<Complex> nrBuffer_;
    int deemphasis_ = 1;  // 50 us (Japan / Europe)
    HalfBandInterpolator<Complex> upsampler_;
    HalfBandDecimator<Complex> downsampler_;  // the discriminator output as (mpx, 0)
    std::vector<Complex> fast_;               // samples at twice the channel rate
    std::vector<float> mpx_;
    Complex prev_{1.0f, 0.0f};
    double pllPhase_ = 0.0;
    double pllFreq_ = kPilotW;
    float lockI_ = 0.0f, lockQ_ = 0.0f;
    float quad_[4] = {};
    float noiseSum_ = 0.0f;
    int noiseCount_ = 0;
    int noiseBlocks_ = 0;
    float noiseDb_ = 0.0f;  // quadrature noise power around the pilot, dB
    float blend_ = 0.0f;
    OnePole deemphL_, deemphR_;
};

// ---- Narrow band FM ---------------------------------------------------------------

class NfmDemodulator : public Demodulator {
public:
    NfmDemodulator() { configure(); }

    double channelRate() const override { return 50000.0; }
    double defaultBandwidth() const override { return 12500.0; }
    double minBandwidth() const override { return 1000.0; }
    double maxBandwidth() const override { return 50000.0; }
    double defaultSnap() const override { return 2500.0; }
    double audioCutoff() const override { return std::clamp(bandwidth_ * 0.5, 3000.0, 16000.0); }

    void setBandwidth(double hz) override { bandwidth_ = hz; }

    void reset() override {
        prev_ = Complex(1.0f, 0.0f);
        dc_.reset();
        deemph_.reset();
        nr_.reset();
    }

    void process(const Complex* input, size_t count, Stereo* out) override {
        const Complex* in = applyIfnr(ifnr_, nr_, nrBuffer_, input, count);
        const float scale = float(channelRate() / (kTwoPi * std::max(bandwidth_ * 0.5, 500.0)));
        for (size_t i = 0; i < count; i++) {
            Complex d = cmulConj(in[i], prev_);
            prev_ = in[i];
            float v = accurateAtan2(d.imag(), d.real()) * scale;
            v -= dc_.process(v);  // remove the DC caused by mistuning
            if (deemphasis_ > 0) v = deemph_.process(v);
            out[i] = Stereo(v);
        }
    }

    void drawSettings() override {
        if (deemphasisCombo("##nfm_deemph", &deemphasis_)) configure();
        if (ifnrCombo("##nfm_ifnr", &ifnr_)) configure();
    }

    void loadState(const nlohmann::json& j) override {
        deemphasis_ = std::clamp(j.value("deemphasis", deemphasis_), 0, 2);
        ifnr_ = std::clamp(j.value("ifnr", ifnr_), 0, 2);
        configure();
    }
    void saveState(nlohmann::json& j) const override {
        j["deemphasis"] = deemphasis_;
        j["ifnr"] = ifnr_;
    }

private:
    // Measured with a 12.5 kHz channel: +2..4 dB SINAD on weak voice signals.
    static constexpr IfnrPreset kIfnrPresets[3] = {{0, 0}, {32, 3}, {32, 2}};

    void configure() {
        deemph_.configureTau(kDeemphasisTau[deemphasis_], channelRate());
        dc_.configureCutoff(20.0, channelRate());
        if (ifnr_ > 0) nr_.configure(kIfnrPresets[ifnr_].fftSize, kIfnrPresets[ifnr_].keepBins);
    }

    double bandwidth_ = 12500.0;
    int deemphasis_ = 0;
    int ifnr_ = 0;
    FmIfNoiseReduction nr_;
    std::vector<Complex> nrBuffer_;
    Complex prev_{1.0f, 0.0f};
    OnePole dc_, deemph_;
};

// ---- AM (envelope detector, carrier normalised) ------------------------------

class AmDemodulator : public Demodulator {
public:
    AmDemodulator() { carrier_.configureCutoff(5.0, channelRate()); }

    double channelRate() const override { return 24000.0; }
    double defaultBandwidth() const override { return 10000.0; }
    double minBandwidth() const override { return 1000.0; }
    double maxBandwidth() const override { return 24000.0; }
    double defaultSnap() const override { return 1000.0; }
    double audioCutoff() const override { return std::max(bandwidth_ * 0.5, 1000.0); }
    void setBandwidth(double hz) override { bandwidth_ = hz; }

    void reset() override { carrier_.reset(); }

    void process(const Complex* in, size_t count, Stereo* out) override {
        for (size_t i = 0; i < count; i++) {
            float env = std::sqrt(magSquared(in[i]));
            if (carrier_.value() <= 0.0f) carrier_.reset(env);
            float c = std::max(carrier_.process(env), 1e-9f);
            out[i] = Stereo(softClip((env / c - 1.0f) * 0.8f));
        }
    }

private:
    double bandwidth_ = 10000.0;
    OnePole carrier_;
};

// ---- SSB / DSB / CW (product detector with AGC) -------------------------------

class LinearDemodulator : public Demodulator {
public:
    enum class Kind { Usb, Lsb, Dsb, Cw };

    explicit LinearDemodulator(Kind kind) : kind_(kind) { configure(); }

    double channelRate() const override { return kind_ == Kind::Cw ? 6000.0 : 24000.0; }
    BandShape bandShape() const override {
        if (kind_ == Kind::Usb) return BandShape::Upper;
        if (kind_ == Kind::Lsb) return BandShape::Lower;
        return BandShape::Center;
    }
    double defaultBandwidth() const override {
        switch (kind_) {
            case Kind::Usb:
            case Kind::Lsb: return 2800.0;
            case Kind::Dsb: return 4600.0;
            case Kind::Cw: return 500.0;
        }
        return 2800.0;
    }
    double minBandwidth() const override { return kind_ == Kind::Cw ? 50.0 : 500.0; }
    double maxBandwidth() const override {
        if (kind_ == Kind::Cw) return 3000.0;
        return kind_ == Kind::Dsb ? 20000.0 : 10000.0;
    }
    double defaultSnap() const override { return kind_ == Kind::Cw ? 10.0 : 100.0; }
    double audioCutoff() const override {
        if (kind_ == Kind::Cw) return bfo_ + bandwidth_ * 0.5 + 200.0;
        if (kind_ == Kind::Dsb) return bandwidth_ * 0.5;
        return bandwidth_;
    }
    void setBandwidth(double hz) override { bandwidth_ = hz; }

    void reset() override {
        agc_.reset();
        bfoNco_.reset();
    }

    void process(const Complex* in, size_t count, Stereo* out) override {
        bfoNco_.setFrequency(kind_ == Kind::Cw ? bfo_ : 0.0, channelRate());
        for (size_t i = 0; i < count; i++) {
            Complex x = in[i];
            float g = agc_.gain(std::sqrt(magSquared(x)));
            if (kind_ == Kind::Cw) x = cmul(x, bfoNco_.next());
            out[i] = Stereo(softClip(x.real() * g));
        }
    }

    void drawSettings() override {
        ui::LabeledRow("AGC");
        const char* speeds[] = {"Fast", "Medium", "Slow"};
        if (ImGui::Combo("##agc", &agcSpeed_, speeds, 3)) configure();
        if (kind_ == Kind::Cw) {
            ui::LabeledRow("Tone");
            float bfo = float(bfo_);
            if (ui::SliderFloat("##bfo", &bfo, 300.0f, 1200.0f, "%.0f Hz")) bfo_ = bfo;
        }
    }

    void loadState(const nlohmann::json& j) override {
        agcSpeed_ = std::clamp(j.value("agc", agcSpeed_), 0, 2);
        bfo_ = std::clamp(j.value("bfo", bfo_), 300.0, 1200.0);
        configure();
    }
    void saveState(nlohmann::json& j) const override {
        j["agc"] = agcSpeed_;
        if (kind_ == Kind::Cw) j["bfo"] = bfo_;
    }

private:
    void configure() {
        const double decay[] = {0.1, 0.4, 1.5};
        agc_.configure(channelRate(), 0.003, decay[agcSpeed_], 0.35f, 80.0f);
    }

    Kind kind_;
    double bandwidth_ = 2800.0;
    double bfo_ = 700.0;
    int agcSpeed_ = 1;
    Agc agc_;
    Nco bfoNco_;
};

// ---- RAW: I on the left channel, Q on the right ------------------------------

class RawDemodulator : public Demodulator {
public:
    double channelRate() const override { return 48000.0; }
    double defaultBandwidth() const override { return 48000.0; }
    double minBandwidth() const override { return 1000.0; }
    double maxBandwidth() const override { return 48000.0; }
    double audioCutoff() const override { return 24000.0; }

    void process(const Complex* in, size_t count, Stereo* out) override {
        for (size_t i = 0; i < count; i++) out[i] = Stereo(in[i].real(), in[i].imag());
    }
};

}  // namespace

std::vector<std::string> builtinDemodulatorNames() {
    return {"WFM", "NFM", "AM", "USB", "LSB", "DSB", "CW", "RAW"};
}

std::unique_ptr<Demodulator> createBuiltinDemodulator(const std::string& name) {
    using K = LinearDemodulator::Kind;
    if (name == "WFM") return std::make_unique<WfmDemodulator>();
    if (name == "NFM") return std::make_unique<NfmDemodulator>();
    if (name == "AM") return std::make_unique<AmDemodulator>();
    if (name == "USB") return std::make_unique<LinearDemodulator>(K::Usb);
    if (name == "LSB") return std::make_unique<LinearDemodulator>(K::Lsb);
    if (name == "DSB") return std::make_unique<LinearDemodulator>(K::Dsb);
    if (name == "CW") return std::make_unique<LinearDemodulator>(K::Cw);
    if (name == "RAW") return std::make_unique<RawDemodulator>();
    return nullptr;
}

}  // namespace SharkSDR::dsp
