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
#include <set>
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
    bool noiseReduction = false;
    float noiseReductionDb = 12.0f;
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
    // The span worth showing and tuning in: the source's usable part of the band.
    double usableBandwidth() const { return rate_.load() * usableRatio_.load(); }
    double usableSpectrumRatio() const { return usableRatio_.load(); }
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
    bool iqCorrection() const { return iqCorrection_.load(); }
    void setIqCorrection(bool on) {
        if (on && !iqCorrection_) iqBalancer_.reset();
        iqCorrection_ = on;
    }

    float signalLevel() const { return signalLevel_.load(); }
    bool squelchOpen() const { return squelchOpen_.load(); }

    // ---- spectrum ----------------------------------------------------------
    SpectrumSettings spectrumSettings() const;
    void setSpectrumSettings(const SpectrumSettings& s);
    void takeSpectrumFrames(std::vector<SpectrumFrame>& out);

    // ---- taps and stream hooks (thread-safe) ---------------------------------
    TapHandle addIqTap(IqTapCallback cb, OwnerId owner);
    TapHandle addAudioTap(AudioTapCallback cb, OwnerId owner);
    TapHandle addStreamHook(ProcessorType type, IqProcessor hook, OwnerId owner);
    TapHandle addStreamHook(ProcessorType type, RealProcessor hook, OwnerId owner);
    TapHandle addStreamHook(ProcessorType type, AudioProcessor hook, OwnerId owner);
    void removeTap(TapHandle handle);  // taps and hooks
    void removeTaps(OwnerId owner);    // taps, hooks and the owner's demodulator bypass

    // The demodulator is bypassed while at least one owner asks for it.
    void setBypassDemodulation(OwnerId owner, bool bypass);
    bool bypassDemodulation() const;

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
    struct StreamHook {
        TapHandle handle = 0;
        OwnerId owner = kCoreOwner;
        ProcessorType type = ProcessorType::RawIQ;
        IqProcessor iq;  // exactly one of these is set, matching `type`
        RealProcessor real;
        AudioProcessor audio;
    };

    void dspLoop();
    void processBlock(Complex* data, size_t count);
    void processSpectrum(const Complex* data, size_t count, double rate, double center);
    void spectrumLoop();
    void computeSpectrumFrame();
    void retune(double centerHz);
    void readSourceRate();  // rate_ and usableRatio_ from the current source
    void clampVfo();
    ModeEntry* findMode(const std::string& name);
    void applyMode(ModeEntry& entry);
    TapHandle addStreamHook(StreamHook hook, OwnerId owner);
    template <typename T>
    void runStreamHooks(ProcessorType type, T* data, size_t count, double sampleRate, double frequency);

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
    std::atomic<double> usableRatio_{1.0};  // Source::usableSpectrumRatio()
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
    std::set<OwnerId> bypassOwners_;
    bool squelchEnabled_ = false;
    float squelchLevel_ = -50.0f;
    std::atomic<float> volume_{0.7f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> dcBlock_{true};
    std::atomic<bool> swapIq_{false};
    std::atomic<bool> iqCorrection_{true};
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
    dsp::IqBalancer iqBalancer_;
    std::atomic<float> dspLoad_{0.0f};
    std::atomic<uint64_t> dropped_{0};

    // spectrum: the DSP thread collects the samples, a worker thread runs the
    // FFT. Frames are skipped while the worker is busy, so a large FFT at a
    // high frame rate can never hold up the audio. Each frame averages the
    // power of several FFTs overlapping by half (Welch's method), so the
    // samples between two frames are analysed too instead of being skipped.
    mutable std::mutex spectrumMutex_;
    SpectrumSettings spectrumSettings_;
    uint64_t spectrumVersion_ = 1;
    uint64_t appliedSpectrumVersion_ = 0;
    std::deque<SpectrumFrame> spectrumFrames_;
    SpectrumSettings fftCfg_;  // DSP thread
    std::vector<Complex> fftRing_;
    size_t fftRingPos_ = 0;
    size_t fftFilled_ = 0;
    size_t fftFresh_ = 0;  // samples since the last frame
    double fftAcc_ = 0.0;

    std::thread spectrumThread_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    bool jobReady_ = false;                  // guarded by jobMutex_
    std::atomic<bool> spectrumBusy_{false};  // the worker owns the job below
    std::vector<Complex> jobSamples_;        // oldest first; FFT s starts at s * jobFftSize_ / 2
    size_t jobFftSize_ = 0, jobSegments_ = 0;
    double jobRate_ = 0.0, jobCenter_ = 0.0;
    dsp::WindowType jobWindow_ = dsp::WindowType::BlackmanHarris;
    // worker thread only
    dsp::Fft fft_;
    std::vector<float> fftWindow_;
    dsp::WindowType fftWindowType_ = dsp::WindowType::BlackmanHarris;
    float fftWindowSum_ = 1.0f;
    std::vector<Complex> fftWork_;
    std::vector<float> fftPower_;

    // taps and stream hooks
    std::mutex tapsMutex_;
    std::vector<IqTap> iqTaps_;
    std::vector<AudioTap> audioTaps_;
    std::vector<StreamHook> streamHooks_;  // in registration order
    TapHandle nextTap_ = 1;
    double blockVfo_ = 0.0;  // DSP thread: VFO frequency of the block being processed
};

}  // namespace SharkSDR
