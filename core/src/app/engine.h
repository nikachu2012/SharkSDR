#pragma once

#include "app/audio_output.h"

#include <SharkSDR/demodulator.h>
#include <SharkSDR/dsp/fft.h>
#include <SharkSDR/dsp/filter.h>
#include <SharkSDR/dsp/receiver.h>
#include <SharkSDR/dsp/ringbuffer.h>
#include <SharkSDR/dsp/window.h>
#include <SharkSDR/host.h>
#include <SharkSDR/source.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace SharkSDR {

using OwnerId = int;
constexpr OwnerId kCoreOwner = 0;

// One FFT result, ready for display.
struct SpectrumFrame {
    std::vector<float> db;  // power per bin in dBFS, index 0 = lowest frequency
    double sampleRate = 0.0;
    double centerFrequency = 0.0;
};

// Audio post filters, stored per demodulation mode.
struct AudioFilterSettings {
    bool highpass = false;
    float highpassHz = 300.0f;
    bool lowpass = false;
    float lowpassHz = 3000.0f;
};

struct SpectrumSettings {
    int fftSize = 8192;
    dsp::WindowType window = dsp::WindowType::BlackmanHarris;
    float frameRate = 30.0f;
};

// Owns the source, the DSP thread, the main receiver and the FFT.
class Engine : public SampleSink {
public:
    explicit Engine(AudioOutput& audio);
    ~Engine() override;

    // ---- sources (UI thread) -----------------------------------------------
    void addSource(std::shared_ptr<Source> source, OwnerId owner);
    void removeSources(OwnerId owner);
    std::vector<std::string> sourceNames() const;
    bool selectSource(const std::string& name);
    Source* currentSource() const { return current_.get(); }
    std::string currentSourceName() const;

    // ---- run state (UI thread) ---------------------------------------------
    bool start();
    void stop();
    bool running() const { return running_.load(); }
    void update();  // once per UI frame
    const std::string& lastError() const { return lastError_; }

    // ---- tuning ------------------------------------------------------------
    double frequency() const { return vfo_.load(); }
    void setFrequency(double hz);
    double centerFrequency() const { return center_.load(); }
    void setCenterFrequency(double hz);
    double sampleRate() const { return rate_.load(); }
    bool centerTuning() const { return centerTuning_; }
    void setCenterTuning(bool on);
    double snap() const;
    void setSnap(double hz);

    // ---- demodulation ------------------------------------------------------
    void registerDemodulator(const std::string& name, DemodulatorFactory factory, OwnerId owner);
    void unregisterDemodulators(OwnerId owner);
    std::vector<std::string> modes() const;
    bool setMode(const std::string& name);
    std::string mode() const;
    double bandwidth() const;
    void setBandwidth(double hz);
    BandShape bandShape() const;
    double minBandwidth() const;
    double maxBandwidth() const;
    void drawDemodulatorSettings();
    std::string demodulatorStatus() const;

    AudioFilterSettings audioFilters() const;  // for the current mode
    void setAudioFilters(const AudioFilterSettings& settings);

    bool squelchEnabled() const { return squelchEnabled_; }
    float squelchLevel() const { return squelchLevel_; }
    void setSquelch(bool enabled, float levelDb);

    float volume() const { return volume_.load(); }
    void setVolume(float v) { volume_ = v; }
    bool muted() const { return muted_.load(); }
    void setMuted(bool m) { muted_ = m; }

    bool dcBlock() const { return dcBlock_.load(); }
    void setDcBlock(bool on) { dcBlock_ = on; }
    bool swapIq() const { return swapIq_.load(); }
    void setSwapIq(bool on) { swapIq_ = on; }

    float signalLevel() const { return signalLevel_.load(); }
    bool squelchOpen() const { return squelchOpen_.load(); }

    // ---- spectrum ----------------------------------------------------------
    SpectrumSettings spectrumSettings() const;
    void setSpectrumSettings(const SpectrumSettings& s);
    void takeSpectrumFrames(std::vector<SpectrumFrame>& out);

    // ---- taps (thread-safe) --------------------------------------------------
    TapHandle addIqTap(IqTapCallback cb, OwnerId owner);
    TapHandle addAudioTap(AudioTapCallback cb, OwnerId owner);
    void removeTap(TapHandle handle);
    void removeTaps(OwnerId owner);

    // ---- statistics ----------------------------------------------------------
    float dspLoad() const { return dspLoad_.load(); }
    uint64_t droppedSamples() const { return dropped_.load(); }

    // ---- persistence ---------------------------------------------------------
    void loadState(const nlohmann::json& j);
    void saveState(nlohmann::json& j);

    // SampleSink
    void pushSamples(const Complex* samples, size_t count) override;
    void sourceStopped(const std::string& reason) override;

private:
    struct SourceEntry {
        std::shared_ptr<Source> source;
        OwnerId owner;
    };
    struct ModeEntry {
        std::string name;
        DemodulatorFactory factory;
        OwnerId owner;
        std::unique_ptr<Demodulator> instance;
    };
    struct IqTap {
        TapHandle handle;
        OwnerId owner;
        IqTapCallback cb;
    };
    struct AudioTap {
        TapHandle handle;
        OwnerId owner;
        AudioTapCallback cb;
    };

    void dspLoop();
    void processBlock(Complex* data, size_t count);
    void processSpectrum(const Complex* data, size_t count, double rate, double center);
    void computeSpectrumFrame(double rate, double center);
    void retune(double centerHz);
    void clampVfo();
    ModeEntry* findMode(const std::string& name);
    void applyMode(ModeEntry& entry);

    AudioOutput& audio_;

    // sources
    std::vector<SourceEntry> sources_;
    std::shared_ptr<Source> current_;
    std::string preferredSource_;
    std::atomic<bool> running_{false};
    std::string lastError_;
    std::mutex stopMutex_;
    std::string stopReason_;
    std::atomic<bool> stopRequested_{false};

    // tuning
    std::atomic<double> vfo_{100.0e6};
    std::atomic<double> center_{100.0e6};
    std::atomic<double> rate_{0.0};
    bool centerTuning_ = false;
    std::map<std::string, double> snaps_;

    // demodulation (chainMutex_ protects receiver_ and the mode table)
    mutable std::recursive_mutex chainMutex_;  // recursive: demodulator UI code may call back into the engine
    std::vector<ModeEntry> modes_;
    std::string mode_;
    std::string preferredMode_ = "WFM";
    std::map<std::string, double> bandwidths_;
    std::map<std::string, AudioFilterSettings> audioFilters_;
    AudioFilterSettings audioFiltersFor(const std::string& mode) const;
    nlohmann::json modeStates_ = nlohmann::json::object();
    dsp::Receiver receiver_;
    bool squelchEnabled_ = false;
    float squelchLevel_ = -50.0f;
    std::atomic<float> volume_{0.7f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> dcBlock_{true};
    std::atomic<bool> swapIq_{false};
    std::atomic<float> signalLevel_{-150.0f};
    std::atomic<bool> squelchOpen_{true};

    // DSP thread
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::mutex dataMutex_;
    std::condition_variable dataCv_;
    dsp::RingBuffer<Complex> iqRing_;
    std::atomic<bool> flushRequested_{false};
    std::vector<Complex> block_;
    std::vector<Stereo> audioBuf_;
    dsp::DcBlocker dcBlocker_;
    double dcRate_ = 0.0;
    std::atomic<float> dspLoad_{0.0f};
    std::atomic<uint64_t> dropped_{0};

    // spectrum (DSP thread state)
    mutable std::mutex spectrumMutex_;
    SpectrumSettings spectrumSettings_;
    uint64_t spectrumVersion_ = 1;
    uint64_t appliedSpectrumVersion_ = 0;
    std::deque<SpectrumFrame> spectrumFrames_;
    SpectrumSettings fftCfg_;
    dsp::Fft fft_;
    std::vector<float> fftWindow_;
    float fftWindowSum_ = 1.0f;
    std::vector<Complex> fftRing_;
    size_t fftRingPos_ = 0;
    size_t fftFilled_ = 0;
    double fftAcc_ = 0.0;
    std::vector<Complex> fftBuf_;

    // taps
    std::mutex tapsMutex_;
    std::vector<IqTap> iqTaps_;
    std::vector<AudioTap> audioTaps_;
    TapHandle nextTap_ = 1;
};

}  // namespace SharkSDR
