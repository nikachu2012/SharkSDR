// Headless checks of the DSP chain: every built-in demodulator is fed with a
// synthetic signal and the frequency content of the audio is verified.

#include <SharkSDR/dsp/demodulators.h>
#include <SharkSDR/dsp/fft.h>
#include <SharkSDR/dsp/noise_reduction.h>
#include <SharkSDR/dsp/receiver.h>
#include <SharkSDR/dsp/resampler.h>
#include <SharkSDR/util/wav.h>

#include <cmath>
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
    testCw();
    testSquelch();
    testIfNoiseReduction();
    testAudioFilters();
    testWav();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
