// Headless checks of the DSP chain: every built-in demodulator is fed with a
// synthetic signal and the frequency content of the audio is verified.

#include <SharkSDR/dsp/agc.h>
#include <SharkSDR/dsp/demodulators.h>
#include <SharkSDR/dsp/fft.h>
#include <SharkSDR/dsp/filter.h>
#include <SharkSDR/dsp/math.h>
#include <SharkSDR/dsp/noise_reduction.h>
#include <SharkSDR/dsp/receiver.h>
#include <SharkSDR/dsp/resampler.h>
#include <SharkSDR/dsp/window.h>
#include <SharkSDR/util/wav.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

using SharkSDR::Complex;
using SharkSDR::Stereo;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) g_failures++;
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kInRate = 2.4e6;
constexpr double kAudioRate = 48000.0;
constexpr double kOffset = 300e3;  // station offset from the IQ center

// Amplitude of the component at `hz` (single-bin DFT, Hann window).
double toneAmplitude(const std::vector<float>& x, double hz, double rate) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    const size_t n = x.size();
    for (size_t i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(n - 1));
        double a = 2.0 * kPi * hz * double(i) / rate;
        re += x[i] * w * std::cos(a);
        im -= x[i] * w * std::sin(a);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

double rms(const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += double(v) * v;
    return x.empty() ? 0.0 : std::sqrt(s / double(x.size()));
}

double db(double ratio) { return 20.0 * std::log10(ratio + 1e-12); }

// Runs `seconds` of synthetic IQ through a Receiver and returns the audio
// (the first `skip` seconds are dropped to let filters/PLLs settle).
struct AudioResult {
    std::vector<float> left, right;
    std::string status;  // demodulator status at the end
};

struct AudioFilters {
    bool highpass = false;
    double highpassHz = 300.0;
    bool lowpass = false;
    double lowpassHz = 3000.0;
};

AudioResult runReceiver(const std::string& mode, double bandwidth, const std::function<Complex(double t)>& signal,
                        double seconds = 2.0, double skip = 0.8, bool squelch = false, float squelchDb = 0.0f,
                        const nlohmann::json& demodState = nlohmann::json::object(), AudioFilters filters = {}) {
    auto demod = SharkSDR::dsp::createBuiltinDemodulator(mode);
    demod->loadState(demodState);
    SharkSDR::dsp::Receiver rx;
    rx.setInputRate(kInRate);
    rx.setAudioRate(kAudioRate);
    rx.setDemodulator(demod.get());
    rx.setBandwidth(bandwidth > 0 ? bandwidth : demod->defaultBandwidth());
    rx.setOffset(kOffset);
    rx.setSquelch(squelch, squelchDb);
    rx.setAudioFilters(filters.highpass, filters.highpassHz, filters.lowpass, filters.lowpassHz);

    AudioResult res;
    std::vector<Complex> block(24000);
    std::vector<Stereo> audio;
    const size_t total = size_t(seconds * kInRate);
    for (size_t pos = 0; pos < total; pos += block.size()) {
        for (size_t i = 0; i < block.size(); i++) block[i] = signal(double(pos + i) / kInRate);
        audio.clear();
        rx.process(block.data(), block.size(), audio);
        for (const auto& f : audio) {
            res.left.push_back(f.l);
            res.right.push_back(f.r);
        }
    }
    const size_t drop = std::min(res.left.size(), size_t(skip * kAudioRate));
    res.left.erase(res.left.begin(), res.left.begin() + std::ptrdiff_t(drop));
    res.right.erase(res.right.begin(), res.right.begin() + std::ptrdiff_t(drop));
    res.status = demod->status();
    return res;
}

Complex phasor(double phase, double amp = 1.0) { return Complex(float(amp * std::cos(phase)), float(amp * std::sin(phase))); }

// ---- tests ----------------------------------------------------------------------------

void testFft() {
    std::puts("FFT");
    const size_t n = 1024;
    SharkSDR::dsp::Fft fft(n);
    std::vector<Complex> x(n);
    for (size_t i = 0; i < n; i++) x[i] = phasor(2.0 * kPi * 100.0 * double(i) / double(n));
    fft.forward(x.data());
    size_t peak = 0;
    for (size_t i = 1; i < n; i++)
        if (std::abs(x[i]) > std::abs(x[peak])) peak = i;
    check(peak == 100, "tone lands in bin 100 (got " + std::to_string(peak) + ")");
    check(std::fabs(std::abs(x[100]) / double(n) - 1.0) < 1e-3, "full scale tone has unit magnitude");

    // Leakage of a tone between two bins, more than 10 bins away from it: the
    // dynamic range of the spectrum next to a strong signal.
    auto leakage = [](SharkSDR::dsp::WindowType type) {
        const size_t m = 4096;
        SharkSDR::dsp::Fft f(m);
        const auto w = SharkSDR::dsp::makeWindow(type, m, true);
        std::vector<Complex> y(m);
        for (size_t i = 0; i < m; i++) y[i] = phasor(2.0 * kPi * 1000.37 * double(i) / double(m)) * w[i];
        f.forward(y.data());
        double top = 0.0, away = 0.0;
        for (size_t k = 0; k < m; k++) {
            const double p = std::norm(std::complex<double>(y[k]));
            top = std::max(top, p);
            if (k + 10 < 1000 || k > 1010) away = std::max(away, p);
        }
        return 10.0 * std::log10(away / top);
    };
    const double bh4 = leakage(SharkSDR::dsp::WindowType::BlackmanHarris);
    const double bh7 = leakage(SharkSDR::dsp::WindowType::BlackmanHarris7);
    check(bh4 < -90.0 && bh7 < -130.0, "leakage 10 bins from a tone: Blackman-Harris " + std::to_string(bh4) +
                                           " dB, 7-term " + std::to_string(bh7) + " dB");
}

void testResampler() {
    std::puts("Resampler");
    SharkSDR::dsp::Resampler<float> rs;
    rs.configure(48000.0, 44100.0, 20000.0, 2000.0);
    std::vector<float> in(48000), out(rs.maxOutput(in.size()));
    for (size_t i = 0; i < in.size(); i++) in[i] = float(std::sin(2.0 * kPi * 1000.0 * double(i) / 48000.0));
    out.resize(rs.process(in.data(), in.size(), out.data()));
    check(std::abs(int(out.size()) - 44100) <= 2, "48k -> 44.1k produces the right number of samples");
    std::vector<float> tail(out.begin() + 4000, out.end());
    const double a = toneAmplitude(tail, 1000.0, 44100.0);
    check(std::fabs(a - 1.0) < 0.02, "1 kHz tone keeps its amplitude (" + std::to_string(a) + ")");

    SharkSDR::dsp::Resampler<float> up;
    up.configure(8000.0, 48000.0, 3500.0, 500.0);
    std::vector<float> in2(8000), out2(up.maxOutput(in2.size()));
    for (size_t i = 0; i < in2.size(); i++) in2[i] = float(std::sin(2.0 * kPi * 440.0 * double(i) / 8000.0));
    out2.resize(up.process(in2.data(), in2.size(), out2.data()));
    std::vector<float> tail2(out2.begin() + 4000, out2.end());
    check(toneAmplitude(tail2, 440.0, 48000.0) > 0.97, "8k -> 48k interpolation keeps a 440 Hz tone");
}

void testWfmStereo() {
    std::puts("WFM stereo");
    // mpx = 0.9 * [(L+R)/2 + (L-R)/2 * sin(2p)] + 0.1 * sin(p), 75 kHz deviation
    double phase = 0.0;
    double lastT = 0.0;
    auto sig = [&](double t) {
        const double l = 0.8 * std::sin(2.0 * kPi * 440.0 * t);
        const double r = 0.8 * std::sin(2.0 * kPi * 660.0 * t);
        const double p = 2.0 * kPi * 19000.0 * t;
        const double mpx = 0.9 * (0.5 * (l + r) + 0.5 * (l - r) * std::sin(2.0 * p)) + 0.1 * std::sin(p);
        phase += 2.0 * kPi * (kOffset + 75000.0 * mpx) * (t - lastT);
        lastT = t;
        return phasor(phase, 0.1);
    };
    auto a = runReceiver("WFM", 200e3, sig, 4.0, 3.0);
    const double l440 = toneAmplitude(a.left, 440.0, kAudioRate), l660 = toneAmplitude(a.left, 660.0, kAudioRate);
    const double r440 = toneAmplitude(a.right, 440.0, kAudioRate), r660 = toneAmplitude(a.right, 660.0, kAudioRate);
    std::printf("    L: 440 Hz %.3f, 660 Hz %.3f | R: 440 Hz %.3f, 660 Hz %.3f | %s\n", l440, l660, r440, r660,
                a.status.c_str());
    check(l440 > 0.2, "left channel carries the 440 Hz tone");
    check(r660 > 0.2, "right channel carries the 660 Hz tone");
    check(db(l440 / l660) > 20.0, "stereo separation left > 20 dB (" + std::to_string(db(l440 / l660)) + " dB)");
    check(db(r660 / r440) > 20.0, "stereo separation right > 20 dB (" + std::to_string(db(r660 / r440)) + " dB)");
}

void testNfm() {
    std::puts("NFM");
    double phase = 0.0, lastT = 0.0;
    auto sig = [&](double t) {
        phase += 2.0 * kPi * (kOffset + 2500.0 * std::sin(2.0 * kPi * 1000.0 * t)) * (t - lastT);
        lastT = t;
        return phasor(phase, 0.05);
    };
    auto a = runReceiver("NFM", 12500.0, sig);
    const double tone = toneAmplitude(a.left, 1000.0, kAudioRate);
    check(tone > 0.2, "1 kHz tone recovered (amplitude " + std::to_string(tone) + ")");
    check(tone / rms(a.left) > 1.3, "output is dominated by the tone");
}

void testAm() {
    std::puts("AM");
    auto sig = [&](double t) {
        const double env = 0.05 * (1.0 + 0.6 * std::sin(2.0 * kPi * 700.0 * t));
        return phasor(2.0 * kPi * kOffset * t, env);
    };
    auto a = runReceiver("AM", 10000.0, sig);
    const double tone = toneAmplitude(a.left, 700.0, kAudioRate);
    check(tone > 0.15, "700 Hz modulation recovered (amplitude " + std::to_string(tone) + ")");
    check(tone / rms(a.left) > 1.3, "output is dominated by the tone");
}

void testSsb() {
    std::puts("SSB");
    // Upper sideband tone at +800 Hz and lower sideband tone at -1500 Hz.
    auto sig = [&](double t) {
        return phasor(2.0 * kPi * (kOffset + 800.0) * t, 0.01) + phasor(2.0 * kPi * (kOffset - 1500.0) * t, 0.01);
    };
    auto usb = runReceiver("USB", 2800.0, sig);
    const double u800 = toneAmplitude(usb.left, 800.0, kAudioRate), u1500 = toneAmplitude(usb.left, 1500.0, kAudioRate);
    check(u800 > 0.05, "USB hears the upper sideband tone");
    check(db(u800 / u1500) > 30.0, "USB rejects the lower sideband (" + std::to_string(db(u800 / u1500)) + " dB)");

    auto lsb = runReceiver("LSB", 2800.0, sig);
    const double l800 = toneAmplitude(lsb.left, 800.0, kAudioRate), l1500 = toneAmplitude(lsb.left, 1500.0, kAudioRate);
    check(l1500 > 0.05, "LSB hears the lower sideband tone");
    check(db(l1500 / l800) > 30.0, "LSB rejects the upper sideband (" + std::to_string(db(l1500 / l800)) + " dB)");

    // Right next to the carrier: a tone 1 kHz into the wanted sideband and one
    // 100 Hz into the other (fixed gain, so the AGC cannot level the residue up).
    const nlohmann::json fixedGain = {{"agcOn", false}, {"gain", 30.0}};
    for (const char* mode : {"USB", "LSB"}) {
        const double side = mode[0] == 'U' ? 1.0 : -1.0;
        auto twoTones = [&](double t) {
            return phasor(2.0 * kPi * (kOffset + side * 1000.0) * t, 0.01) + phasor(2.0 * kPi * (kOffset - side * 100.0) * t, 0.01);
        };
        auto a = runReceiver(mode, 2800.0, twoTones, 2.0, 0.8, false, 0.0f, fixedGain);
        const double rejection = db(toneAmplitude(a.left, 1000.0, kAudioRate) / toneAmplitude(a.left, 100.0, kAudioRate));
        check(rejection > 80.0, std::string(mode) + " rejects the opposite sideband 100 Hz from the carrier (" +
                                    std::to_string(rejection) + " dB)");
    }
}

void testAgc() {
    std::puts("AGC");
    using SharkSDR::dsp::Agc;
    const double rate = 24000.0;
    // -110 dBFS noise, a -40 dBFS tone from 0.5 to 1.0 s, then noise again.
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, float(std::pow(10.0, -110.0 / 20.0) / std::sqrt(2.0)));
    std::vector<Complex> input(size_t(2.0 * rate));
    for (size_t i = 0; i < input.size(); i++) {
        input[i] = Complex(noise(rng), noise(rng));
        if (i >= size_t(0.5 * rate) && i < size_t(1.0 * rate)) input[i] += phasor(2.0 * kPi * 1000.0 * double(i) / rate, 0.01);
    }
    // Output and the gain at each 10 ms block.
    auto run = [&](const Agc::Settings& s, std::vector<float>& gains) {
        Agc agc;
        agc.configure(rate, s);
        std::vector<Complex> y = input;
        for (size_t p = 0; p < y.size(); p += 240) {
            agc.process(&y[p], 240);
            gains.push_back(agc.gainDb());
        }
        return y;
    };
    auto gainAt = [](const std::vector<float>& gains, double t) { return gains[size_t(t * 100.0)]; };

    Agc::Settings s;  // medium: 0.4 s decay, threshold -90 dBFS, output peaks at 0.5
    std::vector<float> gains;
    const auto y = run(s, gains);
    float top = 0.0f;
    double burst = 0.0;
    for (size_t i = 0; i < y.size(); i++) top = std::max(top, std::abs(y[i]));
    for (size_t i = size_t(0.6 * rate); i < size_t(0.95 * rate); i++) burst += std::abs(y[i]);
    burst /= 0.35 * rate;
    check(top < 0.51f, "a signal starting out of the noise does not overshoot (peak " + std::to_string(top) + ")");
    check(std::fabs(burst - 0.5) < 0.01, "it is levelled to 0.5 (" + std::to_string(burst) + ")");
    check(std::fabs(gainAt(gains, 0.4) - (db(0.5) + 90.0)) < 0.5,
          "below the threshold the gain stays at its maximum (" + std::to_string(gainAt(gains, 0.4)) + " dB)");
    const float during = gainAt(gains, 0.9);
    check(gainAt(gains, 1.3) > during + 10.0f, "without hang the gain recovers after the signal (" + std::to_string(during) +
                                                    " -> " + std::to_string(gainAt(gains, 1.3)) + " dB 0.3 s later)");

    s.hang = true;
    gains.clear();
    run(s, gains);
    check(std::fabs(gainAt(gains, 1.3) - gainAt(gains, 0.9)) < 1.5 && gainAt(gains, 1.6) > 80.0f,
          "hang holds the gain for the decay time, then releases it (" + std::to_string(gainAt(gains, 0.9)) + " / " +
              std::to_string(gainAt(gains, 1.3)) + " / " + std::to_string(gainAt(gains, 1.6)) + " dB)");

    s.enabled = false;
    s.manualGainDb = 30.0f;
    gains.clear();
    const auto fixed = run(s, gains);
    check(std::fabs(std::abs(fixed[size_t(0.8 * rate)]) - 0.316f) < 0.01f, "AGC off: fixed gain");

    // Through the receiver: USB tone bursts out of the noise.
    std::mt19937 rng2(9);
    std::normal_distribution<float> n2(0.0f, 2e-5f);
    auto bursts = [&](double t) {
        const bool on = (t >= 0.5 && t < 0.9) || (t >= 1.3 && t < 1.7);
        return phasor(2.0 * kPi * (kOffset + 1000.0) * t, on ? (t < 1.0 ? 0.01 : 0.1) : 0.0) + Complex(n2(rng2), n2(rng2));
    };
    auto a = runReceiver("USB", 2800.0, bursts, 2.0, 0.0);
    float onset = 0.0f;
    for (float v : a.left) onset = std::max(onset, std::fabs(v));
    check(onset < 0.55f, "USB: no overshoot at the start of a transmission (peak " + std::to_string(onset) + ")");
}

void testCw() {
    std::puts("CW");
    auto sig = [&](double t) { return phasor(2.0 * kPi * kOffset * t, 0.01); };
    auto a = runReceiver("CW", 500.0, sig);
    const double tone = toneAmplitude(a.left, 700.0, kAudioRate);
    check(tone > 0.1, "carrier becomes a 700 Hz tone (amplitude " + std::to_string(tone) + ")");
}

void testSquelch() {
    std::puts("Squelch");
    double phase = 0.0, lastT = 0.0;
    auto sig = [&](double t) {
        phase += 2.0 * kPi * (kOffset + 2500.0 * std::sin(2.0 * kPi * 1000.0 * t)) * (t - lastT);
        lastT = t;
        return phasor(phase, 0.001);  // -60 dBFS
    };
    auto closed = runReceiver("NFM", 12500.0, sig, 1.5, 0.5, true, -40.0f);
    check(rms(closed.left) < 1e-6, "squelch at -40 dB mutes a -60 dBFS signal");
    auto open = runReceiver("NFM", 12500.0, sig, 1.5, 0.5, true, -80.0f);
    check(rms(open.left) > 0.05, "squelch at -80 dB lets it through");
}

// SINAD of a tone: everything that is not the tone counts as noise + distortion.
double sinadDb(const std::vector<float>& y, double hz, double rate) {
    double s = 0.0, c = 0.0;
    for (size_t i = 0; i < y.size(); i++) {
        s += y[i] * std::sin(2.0 * kPi * hz * double(i) / rate);
        c += y[i] * std::cos(2.0 * kPi * hz * double(i) / rate);
    }
    s *= 2.0 / double(y.size());
    c *= 2.0 / double(y.size());
    double sig = 0.0, res = 0.0;
    for (size_t i = 0; i < y.size(); i++) {
        const double t = s * std::sin(2.0 * kPi * hz * double(i) / rate) + c * std::cos(2.0 * kPi * hz * double(i) / rate);
        sig += t * t;
        res += (y[i] - t) * (y[i] - t);
    }
    return 10.0 * std::log10(sig / res);
}

// Mono broadcast FM (1 kHz, 90 % deviation) with complex noise for a given CNR in 200 kHz.
std::function<Complex(double)> noisyWfm(double cnrDb, unsigned seed) {
    auto rng = std::make_shared<std::mt19937>(seed);
    auto state = std::make_shared<std::pair<double, double>>(0.0, 0.0);  // phase, last t
    const double amp = 0.1;
    const double sigma = amp * std::sqrt(std::pow(10.0, -cnrDb / 10.0) / (200e3 / kInRate) / 2.0);
    return [=](double t) {
        std::normal_distribution<double> g(0.0, sigma);
        state->first += 2.0 * kPi * (kOffset + 0.9 * 75000.0 * std::sin(2.0 * kPi * 1000.0 * t)) * (t - state->second);
        state->second = t;
        return phasor(state->first, amp) + Complex(float(g(*rng)), float(g(*rng)));
    };
}

void testIfNoiseReduction() {
    std::puts("FM IF noise reduction");
    const nlohmann::json off = {{"stereoMode", 0}, {"ifnr", 0}};
    const nlohmann::json normal = {{"stereoMode", 0}, {"ifnr", 1}};
    auto weakOff = runReceiver("WFM", 150e3, noisyWfm(7.0, 1), 1.6, 0.6, false, 0.0f, off);
    auto weakNr = runReceiver("WFM", 150e3, noisyWfm(7.0, 1), 1.6, 0.6, false, 0.0f, normal);
    const double a = sinadDb(weakOff.left, 1000.0, kAudioRate), b = sinadDb(weakNr.left, 1000.0, kAudioRate);
    // 30 dB with the discriminator at the channel rate; ~35 dB at twice that rate.
    check(a > 33.0, "weak WFM (CNR 7 dB) without IF NR: SINAD " + std::to_string(a) + " dB (oversampled discriminator)");
    check(b > a + 5.0, "weak WFM (CNR 7 dB): SINAD " + std::to_string(a) + " -> " + std::to_string(b) + " dB");
    auto strongOff = runReceiver("WFM", 150e3, noisyWfm(40.0, 2), 1.6, 0.6, false, 0.0f, off);
    auto strongNr = runReceiver("WFM", 150e3, noisyWfm(40.0, 2), 1.6, 0.6, false, 0.0f, normal);
    const double c = sinadDb(strongOff.left, 1000.0, kAudioRate), d = sinadDb(strongNr.left, 1000.0, kAudioRate);
    check(d > c - 1.0, "strong WFM (CNR 40 dB) is not degraded: " + std::to_string(c) + " -> " + std::to_string(d) + " dB");
}

void testAudioFilters() {
    std::puts("Audio filters");
    auto toneLevel = [](bool highpass, double cutoff, double hz) {
        SharkSDR::dsp::ButterworthFilter f;
        f.configure(highpass, cutoff, kAudioRate);
        std::vector<Stereo> x(24000);
        for (size_t i = 0; i < x.size(); i++) x[i] = Stereo(float(std::sin(2.0 * kPi * hz * double(i) / kAudioRate)));
        f.process(x.data(), x.size());
        std::vector<float> tail;
        for (size_t i = 12000; i < x.size(); i++) tail.push_back(x[i].l);
        return db(toneAmplitude(tail, hz, kAudioRate));
    };
    check(toneLevel(true, 300.0, 1000.0) > -0.5, "high-pass 300 Hz passes 1 kHz");
    check(toneLevel(true, 300.0, 120.0) < -40.0, "high-pass 300 Hz removes a 120 Hz CTCSS tone (" + std::to_string(toneLevel(true, 300.0, 120.0)) + " dB)");
    check(toneLevel(false, 3000.0, 1000.0) > -0.5, "low-pass 3 kHz passes 1 kHz");
    check(toneLevel(false, 3000.0, 6000.0) < -40.0, "low-pass 3 kHz removes 6 kHz (" + std::to_string(toneLevel(false, 3000.0, 6000.0)) + " dB)");

    // Through the receiver: NFM carrying 1 kHz + 5 kHz, with a 3 kHz low-pass.
    double phase = 0.0, lastT = 0.0;
    auto sig = [&](double t) {
        phase += 2.0 * kPi * (kOffset + 2000.0 * std::sin(2.0 * kPi * 1000.0 * t) + 2000.0 * std::sin(2.0 * kPi * 5000.0 * t)) * (t - lastT);
        lastT = t;
        return phasor(phase, 0.05);
    };
    AudioFilters lp;
    lp.lowpass = true;
    lp.lowpassHz = 3000.0;
    auto plain = runReceiver("NFM", 25000.0, sig, 1.2, 0.6);
    phase = 0.0, lastT = 0.0;
    auto filtered = runReceiver("NFM", 25000.0, sig, 1.2, 0.6, false, 0.0f, nlohmann::json::object(), lp);
    const double before = toneAmplitude(plain.left, 5000.0, kAudioRate), after = toneAmplitude(filtered.left, 5000.0, kAudioRate);
    check(db(after / before) < -30.0, "receiver low-pass removes the 5 kHz tone (" + std::to_string(db(after / before)) + " dB)");
    check(toneAmplitude(filtered.left, 1000.0, kAudioRate) > 0.9 * toneAmplitude(plain.left, 1000.0, kAudioRate),
          "receiver low-pass keeps the 1 kHz tone");
}

// Amplitude of the complex component at `hz` (single-bin DFT, Hann window).
double complexToneAmplitude(const std::vector<Complex>& x, double hz, double rate) {
    std::complex<double> acc = 0.0;
    double wsum = 0.0;
    const size_t n = x.size();
    for (size_t i = 0; i < n; i++) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(n - 1));
        acc += std::complex<double>(x[i]) * w * std::polar(1.0, -2.0 * kPi * hz * double(i) / rate);
        wsum += w;
    }
    return std::abs(acc) / wsum;
}

void testHalfBand() {
    std::puts("Half-band filters");
    auto tone = [](double hz, double rate, size_t n) {
        std::vector<Complex> x(n);
        for (size_t i = 0; i < n; i++) x[i] = phasor(2.0 * kPi * hz * double(i) / rate);
        return x;
    };
    // 1.2 MHz -> 600 kHz keeping +-100 kHz: 520 kHz would alias to -80 kHz.
    auto decimate = [](std::vector<Complex> x) {
        SharkSDR::dsp::HalfBandDecimator<Complex> d;
        d.configure(100e3, 1.2e6, 100.0);
        x.resize(d.process(x.data(), x.size(), x.data()));
        return std::vector<Complex>(x.begin() + 1000, x.end());
    };
    const double pass = complexToneAmplitude(decimate(tone(50e3, 1.2e6, 60000)), 50e3, 600e3);
    const double alias = complexToneAmplitude(decimate(tone(520e3, 1.2e6, 60000)), -80e3, 600e3);
    check(std::fabs(db(pass)) < 0.01, "decimator passes 50 kHz (" + std::to_string(db(pass)) + " dB)");
    check(db(alias) < -95.0, "decimator removes what would alias into the pass band (" + std::to_string(db(alias)) + " dB)");

    // 300 kHz -> 600 kHz: the image of 50 kHz is at -250 kHz.
    SharkSDR::dsp::HalfBandInterpolator<Complex> up;
    up.configure(100e3, 600e3, 90.0);
    const auto in = tone(50e3, 300e3, 30000);
    std::vector<Complex> out(2 * in.size());
    up.process(in.data(), in.size(), out.data());
    out.erase(out.begin(), out.begin() + 2000);
    check(std::fabs(db(complexToneAmplitude(out, 50e3, 600e3))) < 0.01, "interpolator keeps 50 kHz");
    const double image = complexToneAmplitude(out, -250e3, 600e3);
    check(db(image) < -85.0, "interpolator removes the image (" + std::to_string(db(image)) + " dB)");
}

void testSelectivity() {
    std::puts("Selectivity");
    // Target (1 kHz, full deviation) and a 50 dB stronger station 600 kHz away
    // with a busy programme: that offset folds onto the target in the
    // decimator, so it is only kept out by its stop-band.
    double p1 = 0.0, p2 = 0.0, lastT = 0.0;
    auto sig = [&](double t) {
        const double dt = t - lastT;
        lastT = t;
        const double busy = 0.4 * std::sin(2.0 * kPi * 430.0 * t) + 0.3 * std::sin(2.0 * kPi * 2900.0 * t) + 0.25 * std::sin(2.0 * kPi * 9700.0 * t);
        p1 += 2.0 * kPi * (kOffset + 0.9 * 75000.0 * std::sin(2.0 * kPi * 1000.0 * t)) * dt;
        p2 += 2.0 * kPi * (kOffset + 600e3 + 75000.0 * busy) * dt;
        return phasor(p1, 0.002) + phasor(p2, 0.632);
    };
    auto a = runReceiver("WFM", 150e3, sig, 1.6, 0.6, false, 0.0f, {{"stereoMode", 0}});
    const double s = sinadDb(a.left, 1000.0, kAudioRate);
    check(s > 64.0, "a +50 dB station 600 kHz away does not leak in: SINAD " + std::to_string(s) + " dB");
}

void testIqCorrection() {
    std::puts("IQ correction");
    // A strong station at +300 kHz (nothing at -300 kHz), and equal stations at
    // -800 / +800 kHz that must not be taken for images. I/Q gain 3 %, phase 2 deg.
    const size_t n = size_t(1.5 * kInRate);
    std::vector<Complex> x(n);
    const double g = 1.03, ph = 2.0 * kPi / 180.0;
    for (size_t i = 0; i < n; i++) {
        const double t = double(i) / kInRate;
        const Complex z = phasor(2.0 * kPi * 300e3 * t, 0.3) + phasor(2.0 * kPi * -800e3 * t + 1.0, 0.1) + phasor(2.0 * kPi * 800e3 * t, 0.1);
        x[i] = Complex(z.real(), float(g * (z.imag() * std::cos(ph) + z.real() * std::sin(ph))));
    }
    auto level = [](const std::vector<Complex>& v, double hz) {
        return db(complexToneAmplitude(std::vector<Complex>(v.end() - 65536, v.end()), hz, kInRate));
    };
    const double imageBefore = level(x, -300e3);

    SharkSDR::dsp::IqBalancer bal;
    SharkSDR::dsp::Fft fft(8192);
    std::vector<Complex> spec(8192);
    for (size_t p = 0; p + 24000 <= n; p += 24000) {
        bal.process(x.data() + p, 24000);
        for (size_t k = 0; k < 8192; k++)
            spec[k] = x[p + 24000 - 8192 + k] * float(0.5 - 0.5 * std::cos(2.0 * kPi * double(k) / 8192.0));
        fft.forward(spec.data());
        bal.update(spec.data(), 8192);
    }
    const double station = level(x, 300e3), imageAfter = level(x, -300e3);
    check(station - imageAfter > 70.0, "image of a strong station: " + std::to_string(station - imageBefore) + " -> " +
                                           std::to_string(station - imageAfter) + " dB below it");
    const double lo = level(x, -800e3), hi = level(x, 800e3);
    check(std::fabs(lo - db(0.1)) < 0.5 && std::fabs(hi - db(0.1)) < 0.5,
          "stations on both sides are kept (" + std::to_string(lo) + " / " + std::to_string(hi) + " dB)");
}

void testAudioNoiseReduction() {
    std::puts("Audio noise reduction");
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.03f);
    // Noise only: pushed down by the configured amount.
    SharkSDR::dsp::AudioNoiseReduction nr;
    nr.configure(kAudioRate, 12.0f);
    std::vector<Stereo> x(size_t(6 * kAudioRate));
    for (auto& v : x) v = Stereo(noise(rng), noise(rng));
    auto y = x;
    nr.process(y.data(), y.size());
    double pin = 0.0, pout = 0.0;
    for (size_t i = size_t(3 * kAudioRate); i < x.size(); i++) {
        pin += double(x[i].l) * x[i].l + double(x[i].r) * x[i].r;
        pout += double(y[i].l) * y[i].l + double(y[i].r) * y[i].r;
    }
    const double reduction = 10.0 * std::log10(pout / pin);
    check(reduction < -10.5 && reduction > -13.5, "noise is reduced by ~12 dB (" + std::to_string(reduction) + " dB)");

    // A steady tone well above the noise is not mistaken for noise.
    nr.configure(kAudioRate, 12.0f);
    for (size_t i = 0; i < x.size(); i++) x[i] = Stereo(float(0.3 * std::sin(2.0 * kPi * 1000.0 * double(i) / kAudioRate)) + noise(rng));
    nr.process(x.data(), x.size());
    std::vector<float> tail;
    for (size_t i = size_t(3 * kAudioRate); i < x.size(); i++) tail.push_back(x[i].l);
    const double tone = toneAmplitude(tail, 1000.0, kAudioRate);
    check(tone > 0.29 && tone < 0.31, "a steady tone keeps its level (" + std::to_string(tone) + " of 0.3)");
}

void testAtan2() {
    std::puts("atan2");
    double worst = 0.0;
    for (int i = 0; i < 100000; i++) {
        const double a = -kPi + 2.0 * kPi * (double(i) + 0.5) / 100000.0;
        for (double r : {1e-6, 0.01, 1.0, 1000.0}) {
            const float y = float(r * std::sin(a)), x = float(r * std::cos(a));
            worst = std::max(worst, std::fabs(double(SharkSDR::dsp::accurateAtan2(y, x)) - std::atan2(double(y), double(x))));
        }
    }
    char msg[64];
    std::snprintf(msg, sizeof(msg), "accurateAtan2 max error %.1e rad", worst);
    check(worst < 1e-6, msg);
}

void testAudioCutoffFollowsBandwidth() {
    std::puts("Audio cut-off");
    // NFM with a 10 kHz tone: cut off at 12.5 kHz bandwidth (6.25 kHz audio),
    // heard after widening to 25 kHz while running.
    auto demod = SharkSDR::dsp::createBuiltinDemodulator("NFM");
    SharkSDR::dsp::Receiver rx;
    rx.setInputRate(kInRate);
    rx.setAudioRate(kAudioRate);
    rx.setDemodulator(demod.get());
    rx.setBandwidth(12500.0);
    rx.setOffset(kOffset);
    double phase = 0.0;
    std::vector<Complex> block(24000);
    std::vector<Stereo> audio;
    std::vector<float> narrow, wide;
    for (size_t pos = 0; pos < size_t(1.6 * kInRate); pos += block.size()) {
        if (pos == size_t(0.8 * kInRate)) rx.setBandwidth(25000.0);
        for (size_t i = 0; i < block.size(); i++) {
            const double t = double(pos + i) / kInRate;
            phase += 2.0 * kPi * (kOffset + 2000.0 * std::sin(2.0 * kPi * 10000.0 * t)) / kInRate;
            block[i] = phasor(phase, 0.05);
        }
        audio.clear();
        rx.process(block.data(), block.size(), audio);
        for (const auto& f : audio) (pos < size_t(0.8 * kInRate) ? narrow : wide).push_back(f.l);
    }
    narrow.erase(narrow.begin(), narrow.begin() + 12000);
    wide.erase(wide.begin(), wide.begin() + 12000);
    const double before = toneAmplitude(narrow, 10000.0, kAudioRate), after = toneAmplitude(wide, 10000.0, kAudioRate);
    check(db(before) < -40.0 && after > 0.1,
          "10 kHz audio: " + std::to_string(db(before)) + " dB at 12.5 kHz bandwidth, " + std::to_string(db(after)) + " dB at 25 kHz");
}

void testStreamHooks() {
    std::puts("Stream hooks");
    auto wfm = SharkSDR::dsp::createBuiltinDemodulator("WFM");
    auto nfm = SharkSDR::dsp::createBuiltinDemodulator("NFM");
    SharkSDR::dsp::Receiver rx;
    rx.setInputRate(kInRate);
    rx.setAudioRate(kAudioRate);
    rx.setDemodulator(wfm.get());
    rx.setBandwidth(150e3);
    rx.setOffset(kOffset);

    struct Seen {
        size_t calls = 0, samples = 0;
        double rate = 0.0;
        void note(size_t n, double r) { calls++, samples += n, rate = r; }
    } channel, mpx, demodulated, audio;
    bool muteMpx = false;
    SharkSDR::dsp::Receiver::StreamHooks hooks;
    hooks.channel = [&](Complex*, size_t n, double r) { channel.note(n, r); };
    hooks.mpx = [&](float* s, size_t n, double r) {
        mpx.note(n, r);
        if (muteMpx) std::fill(s, s + n, 0.0f);
    };
    hooks.demodulated = [&](Stereo*, size_t n, double r) { demodulated.note(n, r); };
    hooks.audio = [&](Stereo*, size_t n, double r) { audio.note(n, r); };
    rx.setStreamHooks(hooks);

    // A WFM station modulated with a 1 kHz tone; returns the left channel
    // without its first 0.2 s.
    size_t t = 0;
    double phase = 0.0;
    auto run = [&](double seconds) {
        std::vector<Complex> block(24000);
        std::vector<Stereo> buf;
        std::vector<float> out;
        for (size_t pos = 0; pos < size_t(seconds * kInRate); pos += block.size()) {
            for (auto& v : block) {
                phase += 2.0 * kPi * (kOffset + 37500.0 * std::sin(2.0 * kPi * 1000.0 * double(t++) / kInRate)) / kInRate;
                v = phasor(phase, 0.1);
            }
            buf.clear();
            rx.process(block.data(), block.size(), buf);
            for (const auto& f : buf) out.push_back(f.l);
        }
        out.erase(out.begin(), out.begin() + std::min(out.size(), size_t(0.2 * kAudioRate)));
        return out;
    };

    const double tone = toneAmplitude(run(1.0), 1000.0, kAudioRate);
    char rates[160];
    std::snprintf(rates, sizeof(rates), "channel %.0f, mpx %.0f, demodulated %.0f, audio %.0f samples/s", channel.rate, mpx.rate,
                  demodulated.rate, audio.rate);
    check(channel.rate == 250e3 && mpx.rate == 250e3 && demodulated.rate == 250e3 && audio.rate == kAudioRate &&
              std::abs(double(channel.samples) - 250e3) < 2500.0 && mpx.samples == channel.samples &&
              demodulated.samples == channel.samples && std::abs(double(audio.samples) - kAudioRate) < 480.0,
          std::string("every hook sees the whole stream at its rate (") + rates + ")");

    muteMpx = true;
    const double muted = rms(run(0.5));
    muteMpx = false;
    check(tone > 0.2 && muted < 1e-4,
          "the demodulator decodes the MPX as the hook left it (tone " + std::to_string(tone) + ", zeroed MPX " +
              std::to_string(muted) + " rms)");

    rx.setBypassDemodulation(true);
    const size_t mpxBefore = mpx.calls, channelBefore = channel.calls, demodulatedBefore = demodulated.calls;
    const double bypassed = rms(run(0.5));
    check(bypassed < 1e-4 && mpx.calls == mpxBefore && channel.calls > channelBefore && demodulated.calls > demodulatedBefore,
          "bypassing the demodulator: silent audio, channel IQ still delivered (" + std::to_string(bypassed) + " rms)");
    rx.setBypassDemodulation(false);
    check(toneAmplitude(run(0.6), 1000.0, kAudioRate) > 0.2, "the tone is back after the bypass");

    rx.setDemodulator(nfm.get());
    const size_t mpxCalls = mpx.calls;
    run(0.3);
    const size_t nfmCalls = mpx.calls - mpxCalls;
    rx.setDemodulator(wfm.get());
    run(0.3);
    check(nfmCalls == 0 && mpx.calls > mpxCalls, "the MPX hook follows the demodulator (none from NFM, back with WFM)");
}

void testWav() {
    std::puts("WAV");
    const auto path = std::filesystem::temp_directory_path() / "sharksdr_test.wav";
    {
        SharkSDR::WavWriter w;
        check(w.open(path, 48000, 2, SharkSDR::WavFormat::S16), "open for writing");
        std::vector<float> data(2000);
        for (size_t i = 0; i < data.size(); i++) data[i] = float(i % 200) / 200.0f - 0.5f;
        w.write(data.data(), 1000);
    }
    SharkSDR::WavReader r;
    check(r.open(path), "open for reading");
    check(r.sampleRate() == 48000 && r.channels() == 2 && r.totalFrames() == 1000, "header round trip");
    std::vector<float> back(2000);
    check(r.read(back.data(), 1000) == 1000, "read all frames");
    check(std::fabs(back[123] - (float(123 % 200) / 200.0f - 0.5f)) < 1e-3, "sample values round trip");
    r.close();
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    testFft();
    testResampler();
    testWfmStereo();
    testNfm();
    testAm();
    testSsb();
    testAgc();
    testCw();
    testSquelch();
    testIfNoiseReduction();
    testAudioFilters();
    testHalfBand();
    testSelectivity();
    testIqCorrection();
    testAudioNoiseReduction();
    testAtan2();
    testAudioCutoffFollowsBandwidth();
    testStreamHooks();
    testWav();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
