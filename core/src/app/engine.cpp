#include "app/engine.h"

#include <SharkSDR/dsp/agc.h>
#include <SharkSDR/dsp/demodulators.h>
#include <SharkSDR/dsp/math.h>
#include <SharkSDR/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <type_traits>

namespace SharkSDR {

namespace {
constexpr size_t kMaxSpectrumQueue = 16;
// Welch averaging of the spectrum: at most this many FFTs per frame, and at
// most this many FFT points in all (8 x 8192), which bounds the CPU time.
constexpr size_t kMaxSpectrumSegments = 16;
constexpr size_t kSpectrumPointsPerFrame = 65536;

bool isIqPoint(ProcessorType t) { return t == ProcessorType::RawIQ || t == ProcessorType::DecimatedAndFilteredIQ; }
bool isAudioPoint(ProcessorType t) {
    return t == ProcessorType::DemodulatorOutput || t == ProcessorType::FilteredAudioOutput || t == ProcessorType::MonitorAF;
}

double usableRatio(const Source& source) { return std::clamp(source.usableSpectrumRatio(), 0.1, 1.0); }
}  // namespace

// DSP thread.
template <typename T>
void Engine::runStreamHooks(ProcessorType type, T* data, size_t count, double sampleRate, double frequency) {
    const StreamInfo info{type, sampleRate, frequency};
    std::lock_guard<std::mutex> lock(tapsMutex_);
    for (auto& h : streamHooks_) {
        if (h.type != type) continue;
        if constexpr (std::is_same_v<T, Complex>) h.iq(data, count, info);
        else if constexpr (std::is_same_v<T, float>) h.real(data, count, info);
        else h.audio(data, count, info);
    }
}

Engine::Engine(AudioOutput& audio) : audio_(audio) {
    iqRing_.resize(size_t(1) << 22);
    block_.resize(size_t(1) << 18);
    for (const auto& name : dsp::builtinDemodulatorNames())
        registerDemodulator(name, [name] { return dsp::createBuiltinDemodulator(name); }, kCoreOwner);

    // The stream hooks inside the receive chain (RawIQ and MonitorAF run in processBlock()).
    dsp::Receiver::StreamHooks hooks;
    hooks.channel = [this](Complex* s, size_t n, double rate) {
        runStreamHooks(ProcessorType::DecimatedAndFilteredIQ, s, n, rate, blockVfo_);
    };
    hooks.mpx = [this](float* s, size_t n, double rate) { runStreamHooks(ProcessorType::FMMPX, s, n, rate, 0.0); };
    hooks.demodulated = [this](Stereo* s, size_t n, double rate) {
        runStreamHooks(ProcessorType::DemodulatorOutput, s, n, rate, 0.0);
    };
    hooks.audio = [this](Stereo* s, size_t n, double rate) {
        runStreamHooks(ProcessorType::FilteredAudioOutput, s, n, rate, 0.0);
    };
    receiver_.setStreamHooks(std::move(hooks));

    thread_ = std::thread([this] { dspLoop(); });
    spectrumThread_ = std::thread([this] { spectrumLoop(); });
}

Engine::~Engine() {
    stop();
    quit_ = true;
    dataCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        jobCv_.notify_all();
    }
    if (spectrumThread_.joinable()) spectrumThread_.join();
}

// ---- sources ----------------------------------------------------------------------

void Engine::addSource(std::shared_ptr<Source> source, OwnerId owner) {
    if (!source) return;
    const std::string name = source->name();
    for (const auto& e : sources_) {
        if (e.source->name() == name) {
            logWarning("Engine", "Duplicate source name '" + name + "' ignored");
            return;
        }
    }
    sources_.push_back({std::move(source), owner});
    if (!current_ && (preferredSource_.empty() || preferredSource_ == name)) selectSource(name);
}

void Engine::removeSources(OwnerId owner) {
    if (current_) {
        for (const auto& e : sources_) {
            if (e.owner == owner && e.source == current_) {
                stop();
                current_.reset();
                rate_ = 0.0;
                break;
            }
        }
    }
    sources_.erase(std::remove_if(sources_.begin(), sources_.end(), [&](const SourceEntry& e) { return e.owner == owner; }),
                   sources_.end());
}

std::vector<std::string> Engine::sourceNames() const {
    std::vector<std::string> names;
    for (const auto& e : sources_) names.push_back(e.source->name());
    return names;
}

std::string Engine::currentSourceName() const { return current_ ? current_->name() : std::string(); }

bool Engine::selectSource(const std::string& name) {
    for (const auto& e : sources_) {
        if (e.source->name() != name) continue;
        if (e.source == current_) return true;
        const bool wasRunning = running_;
        stop();
        current_ = e.source;
        preferredSource_ = name;
        current_->onSelected();
        readSourceRate();
        if (current_->tunable()) current_->tune(center_.load());
        else center_ = current_->frequency();
        clampVfo();
        if (wasRunning) start();
        return true;
    }
    return false;
}

// ---- run state --------------------------------------------------------------------

bool Engine::start() {
    if (running_) return true;
    if (!current_) {
        lastError_ = "No source selected";
        return false;
    }
    lastError_.clear();
    flushRequested_ = true;
    readSourceRate();
    if (current_->tunable()) current_->tune(center_.load());
    stopRequested_ = false;
    running_ = true;
    if (!current_->start(*this)) {
        running_ = false;
        lastError_ = current_->lastError();
        if (lastError_.empty()) lastError_ = "Failed to start " + current_->name();
        logError("Engine", lastError_);
        return false;
    }
    logInfo("Engine", "Started " + current_->name());
    return true;
}

void Engine::stop() {
    if (!running_) return;
    running_ = false;
    if (current_) current_->stop();
    flushRequested_ = true;
    dataCv_.notify_all();
    logInfo("Engine", "Stopped");
}

void Engine::update() {
    if (stopRequested_.exchange(false)) {
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(stopMutex_);
            reason = stopReason_;
        }
        stop();
        lastError_ = reason;
        if (!reason.empty()) logWarning("Engine", "Source stopped: " + reason);
    }
    if (current_) {
        const double r = current_->sampleRate();
        if (r != rate_.load() || usableRatio(*current_) != usableRatio_.load()) {
            readSourceRate();
            clampVfo();
        }
        if (!current_->tunable()) {
            const double c = current_->frequency();
            if (c != center_.load()) {
                center_ = c;
                clampVfo();
            }
        }
    }
}

void Engine::pushSamples(const Complex* samples, size_t count) {
    size_t written = iqRing_.write(samples, count);
    if (written < count) dropped_ += count - written;
    dataCv_.notify_one();
}

void Engine::sourceStopped(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(stopMutex_);
        stopReason_ = reason;
    }
    stopRequested_ = true;
}

// ---- tuning -----------------------------------------------------------------------

void Engine::retune(double centerHz) {
    centerHz = std::max(0.0, centerHz);
    center_ = centerHz;
    if (current_ && current_->tunable()) current_->tune(centerHz);
}

void Engine::readSourceRate() {
    rate_ = current_->sampleRate();
    usableRatio_ = usableRatio(*current_);
}

void Engine::clampVfo() {
    if (rate_.load() <= 0.0) return;
    const double c = center_.load(), half = usableBandwidth() * 0.5;
    vfo_ = std::clamp(vfo_.load(), c - half, c + half);
}

void Engine::setFrequency(double hz) {
    hz = std::max(0.0, hz);
    const double rate = rate_.load();
    const double c = center_.load();
    const double half = usableBandwidth() * 0.5;
    if (current_ && !current_->tunable()) {
        vfo_ = rate > 0.0 ? std::clamp(hz, c - half, c + half) : hz;
        return;
    }
    if (centerTuning_) {
        retune(hz);
        vfo_ = hz;
        return;
    }
    const double margin = std::min(bandwidth() * 0.5, half * 0.5);
    if (rate <= 0.0 || hz - margin < c - half || hz + margin > c + half) {
        // Outside the visible band: move the center, keeping the VFO at the
        // same relative position so the spectrum scrolls smoothly.
        double off = vfo_.load() - c;
        if (std::fabs(off) + margin > half) off = 0.0;
        retune(hz - off);
    }
    vfo_ = hz;
}

void Engine::setCenterFrequency(double hz) {
    retune(hz);
    if (centerTuning_) vfo_ = center_.load();
    clampVfo();
}

void Engine::setCenterTuning(bool on) {
    centerTuning_ = on;
    if (on) retune(vfo_.load());
}

double Engine::snap() const {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    auto it = snaps_.find(mode_);
    if (it != snaps_.end()) return it->second;
    Demodulator* d = receiver_.demodulator();
    return d ? d->defaultSnap() : 1000.0;
}

void Engine::setSnap(double hz) {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    snaps_[mode_] = std::max(1.0, hz);
}

// ---- demodulation -----------------------------------------------------------------

Engine::ModeEntry* Engine::findMode(const std::string& name) {
    for (auto& m : modes_)
        if (m.name == name) return &m;
    return nullptr;
}

void Engine::registerDemodulator(const std::string& name, DemodulatorFactory factory, OwnerId owner) {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    if (findMode(name)) {
        logWarning("Engine", "Duplicate demodulator '" + name + "' ignored");
        return;
    }
    modes_.push_back({name, std::move(factory), owner, nullptr});
    // Activate it if nothing is active yet, or if it is the mode the user had
    // selected last time (e.g. a mode provided by a plugin loaded later).
    if (mode_.empty() || (name == preferredMode_ && mode_ != name)) {
        const std::string preferred = preferredMode_;
        applyMode(modes_.back());
        if (!preferred.empty()) preferredMode_ = preferred;
    }
}

void Engine::unregisterDemodulators(OwnerId owner) {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    bool currentRemoved = false;
    for (auto& m : modes_) {
        if (m.owner != owner) continue;
        if (m.instance) {
            nlohmann::json state = nlohmann::json::object();
            m.instance->saveState(state);
            modeStates_[m.name] = state;
        }
        if (m.name == mode_) currentRemoved = true;
    }
    if (currentRemoved) {
        receiver_.setDemodulator(nullptr);
        mode_.clear();
    }
    modes_.erase(std::remove_if(modes_.begin(), modes_.end(), [&](const ModeEntry& m) { return m.owner == owner; }),
                 modes_.end());
    if (currentRemoved && !modes_.empty()) {
        const std::string keep = preferredMode_;
        ModeEntry* fallback = findMode("NFM");
        applyMode(fallback ? *fallback : modes_.front());
        preferredMode_ = keep;  // switch back if the plugin is loaded again
    }
}

std::vector<std::string> Engine::modes() const {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    std::vector<std::string> out;
    for (const auto& m : modes_) out.push_back(m.name);
    return out;
}

void Engine::applyMode(ModeEntry& entry) {
    if (!entry.instance) {
        entry.instance = entry.factory ? entry.factory() : nullptr;
        if (!entry.instance) {
            logError("Engine", "Demodulator factory for '" + entry.name + "' failed");
            return;
        }
        auto it = modeStates_.find(entry.name);
        if (it != modeStates_.end()) entry.instance->loadState(*it);
    }
    Demodulator* d = entry.instance.get();
    mode_ = entry.name;
    preferredMode_ = mode_;
    auto bw = bandwidths_.find(mode_);
    double hz = bw != bandwidths_.end() ? bw->second : d->defaultBandwidth();
    hz = std::clamp(hz, d->minBandwidth(), d->maxBandwidth());
    receiver_.setDemodulator(d);
    receiver_.setBandwidth(hz);
    const AudioFilterSettings f = audioFiltersFor(mode_);
    receiver_.setAudioFilters(f.highpass, f.highpassHz, f.lowpass, f.lowpassHz);
    receiver_.setNoiseReduction(f.noiseReduction, f.noiseReductionDb);
}

AudioFilterSettings Engine::audioFiltersFor(const std::string& mode) const {
    auto it = audioFilters_.find(mode);
    if (it != audioFilters_.end()) return it->second;
    AudioFilterSettings f;  // defaults: CTCSS-style high-pass, voice / broadcast low-pass
    f.lowpassHz = mode == "WFM" ? 10000.0f : mode == "RAW" ? 20000.0f : 3000.0f;
    return f;
}

AudioFilterSettings Engine::audioFilters() const {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    return audioFiltersFor(mode_);
}

void Engine::setAudioFilters(const AudioFilterSettings& settings) {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    AudioFilterSettings f = settings;
    f.highpassHz = std::clamp(f.highpassHz, 10.0f, 5000.0f);
    f.lowpassHz = std::clamp(f.lowpassHz, 200.0f, 23000.0f);
    f.noiseReductionDb = std::clamp(f.noiseReductionDb, 3.0f, 30.0f);
    audioFilters_[mode_] = f;
    receiver_.setAudioFilters(f.highpass, f.highpassHz, f.lowpass, f.lowpassHz);
    receiver_.setNoiseReduction(f.noiseReduction, f.noiseReductionDb);
}

bool Engine::setMode(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    ModeEntry* m = findMode(name);
    if (!m) {
        preferredMode_ = name;
        return false;
    }
    if (name != mode_) applyMode(*m);
    return true;
}

std::string Engine::mode() const {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    return mode_;
}

double Engine::bandwidth() const {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    return receiver_.bandwidth();
}

void Engine::setBandwidth(double hz) {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    Demodulator* d = receiver_.demodulator();
    if (!d) return;
    hz = std::clamp(hz, d->minBandwidth(), d->maxBandwidth());
    receiver_.setBandwidth(hz);
    bandwidths_[mode_] = hz;
}

BandShape Engine::bandShape() const {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    Demodulator* d = receiver_.demodulator();
    return d ? d->bandShape() : BandShape::Center;
}

double Engine::minBandwidth() const {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    Demodulator* d = receiver_.demodulator();
    return d ? d->minBandwidth() : 100.0;
}

double Engine::maxBandwidth() const {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    Demodulator* d = receiver_.demodulator();
    return d ? d->maxBandwidth() : 1e6;
}

void Engine::drawDemodulatorSettings() {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    if (Demodulator* d = receiver_.demodulator()) d->drawSettings();
}

std::string Engine::demodulatorStatus() const {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    Demodulator* d = receiver_.demodulator();
    return d ? d->status() : std::string();
}

void Engine::setSquelch(bool enabled, float levelDb) {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    squelchEnabled_ = enabled;
    squelchLevel_ = levelDb;
    receiver_.setSquelch(enabled, levelDb);
}

// ---- spectrum ---------------------------------------------------------------------

SpectrumSettings Engine::spectrumSettings() const {
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    return spectrumSettings_;
}

void Engine::setSpectrumSettings(const SpectrumSettings& s) {
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    spectrumSettings_ = s;
    spectrumSettings_.fftSize = std::clamp(s.fftSize, 256, 1 << 20);
    spectrumSettings_.frameRate = std::clamp(s.frameRate, 1.0f, 120.0f);
    spectrumVersion_++;
}

void Engine::takeSpectrumFrames(std::vector<SpectrumFrame>& out) {
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    for (auto& f : spectrumFrames_) out.push_back(std::move(f));
    spectrumFrames_.clear();
}

void Engine::processSpectrum(const Complex* data, size_t count, double rate, double center) {
    bool rebuild = false;
    {
        std::lock_guard<std::mutex> lock(spectrumMutex_);
        if (spectrumVersion_ != appliedSpectrumVersion_) {
            fftCfg_ = spectrumSettings_;
            appliedSpectrumVersion_ = spectrumVersion_;
            rebuild = true;
        }
    }
    const size_t n = size_t(fftCfg_.fftSize);
    const size_t hop = n / 2;
    const size_t maxSegments = std::clamp<size_t>(kSpectrumPointsPerFrame / n, 1, kMaxSpectrumSegments);
    const size_t capacity = n + hop * (maxSegments - 1);
    if (rebuild || fftRing_.size() != capacity) {
        fftRing_.assign(capacity, Complex());
        fftRingPos_ = 0;
        fftFilled_ = 0;
        fftFresh_ = 0;
        fftAcc_ = 0.0;
    }

    const double interval = std::max(1.0, rate / double(fftCfg_.frameRate));
    size_t i = 0;
    while (i < count) {
        const size_t need = std::max<size_t>(1, size_t(std::ceil(interval - fftAcc_)));
        const size_t take = std::min(count - i, need);
        // Append to the ring (only the last `capacity` samples matter).
        size_t start = take > capacity ? take - capacity : 0;
        for (size_t k = start; k < take; k++) {
            fftRing_[fftRingPos_] = data[i + k];
            if (++fftRingPos_ == capacity) fftRingPos_ = 0;
        }
        fftFilled_ = std::min(capacity, fftFilled_ + take);
        fftFresh_ += take;
        i += take;
        fftAcc_ += double(take);
        if (fftAcc_ >= interval) {
            fftAcc_ -= interval;
            // Hand the latest samples to the worker, unless it is still busy:
            // as many half-overlapping FFTs as cover the samples since the
            // last frame (at least one, which may reach back into it).
            if (fftFilled_ >= n && !spectrumBusy_.load()) {
                const size_t fresh = std::min(std::max(fftFresh_, n), fftFilled_);
                const size_t segments = 1 + (fresh - n) / hop;
                const size_t len = n + (segments - 1) * hop;
                const size_t first = (fftRingPos_ + capacity - len) % capacity;
                jobSamples_.resize(len);
                for (size_t k = 0, p = first; k < len; k++) {
                    jobSamples_[k] = fftRing_[p];
                    if (++p == capacity) p = 0;
                }
                jobFftSize_ = n;
                jobSegments_ = segments;
                fftFresh_ = 0;
                jobRate_ = rate;
                jobCenter_ = center;
                jobWindow_ = fftCfg_.window;
                spectrumBusy_ = true;
                std::lock_guard<std::mutex> lock(jobMutex_);
                jobReady_ = true;
                jobCv_.notify_one();
            }
        }
    }
}

void Engine::spectrumLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(jobMutex_);
            jobCv_.wait(lock, [&] { return jobReady_ || quit_.load(); });
            if (quit_) break;
            jobReady_ = false;
        }
        computeSpectrumFrame();
        spectrumBusy_ = false;
    }
}

void Engine::computeSpectrumFrame() {
    const size_t n = jobFftSize_;
    if (fft_.size() != n || fftWindow_.size() != n || fftWindowType_ != jobWindow_) {
        fft_.resize(n);
        fftWindow_ = dsp::makeWindow(jobWindow_, n, true);
        fftWindowType_ = jobWindow_;
        double sum = 0.0;
        for (float w : fftWindow_) sum += w;
        fftWindowSum_ = float(sum);
    }
    // Average power of the segments (Welch's method).
    fftWork_.resize(n);
    fftPower_.assign(n, 0.0f);
    for (size_t s = 0; s < jobSegments_; s++) {
        const Complex* x = &jobSamples_[s * (n / 2)];
        for (size_t k = 0; k < n; k++) fftWork_[k] = x[k] * fftWindow_[k];
        fft_.forward(fftWork_.data());
        for (size_t k = 0; k < n; k++) fftPower_[k] += magSquared(fftWork_[k]);
    }
    if (iqCorrection_) iqBalancer_.update(fftWork_.data(), n);  // the latest segment

    SpectrumFrame frame;
    frame.db.resize(n);
    frame.sampleRate = jobRate_;
    frame.centerFrequency = jobCenter_;
    // Normalised so that a full-scale tone reads 0 dBFS.
    const float norm = 1.0f / (fftWindowSum_ * fftWindowSum_ * float(std::max<size_t>(jobSegments_, 1)));
    const size_t half = n / 2;
    for (size_t k = 0; k < n; k++) {
        size_t src = k + half;
        if (src >= n) src -= n;
        frame.db[k] = dsp::powerToDb(fftPower_[src] * norm);
    }
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    spectrumFrames_.push_back(std::move(frame));
    while (spectrumFrames_.size() > kMaxSpectrumQueue) spectrumFrames_.pop_front();
}

// ---- taps -------------------------------------------------------------------------

TapHandle Engine::addIqTap(IqTapCallback cb, OwnerId owner) {
    std::lock_guard<std::mutex> lock(tapsMutex_);
    TapHandle h = nextTap_++;
    iqTaps_.push_back({h, owner, std::move(cb)});
    return h;
}

TapHandle Engine::addAudioTap(AudioTapCallback cb, OwnerId owner) {
    std::lock_guard<std::mutex> lock(tapsMutex_);
    TapHandle h = nextTap_++;
    audioTaps_.push_back({h, owner, std::move(cb)});
    return h;
}

TapHandle Engine::addStreamHook(ProcessorType type, IqProcessor hook, OwnerId owner) {
    StreamHook h;
    h.type = type;
    h.iq = std::move(hook);
    return addStreamHook(std::move(h), owner);
}

TapHandle Engine::addStreamHook(ProcessorType type, RealProcessor hook, OwnerId owner) {
    StreamHook h;
    h.type = type;
    h.real = std::move(hook);
    return addStreamHook(std::move(h), owner);
}

TapHandle Engine::addStreamHook(ProcessorType type, AudioProcessor hook, OwnerId owner) {
    StreamHook h;
    h.type = type;
    h.audio = std::move(hook);
    return addStreamHook(std::move(h), owner);
}

TapHandle Engine::addStreamHook(StreamHook hook, OwnerId owner) {
    const bool matches = hook.iq ? isIqPoint(hook.type)
                         : hook.real ? hook.type == ProcessorType::FMMPX
                                     : hook.audio && isAudioPoint(hook.type);
    if (!matches) {
        logError("Engine", "Stream hook ignored: its sample type does not match the point (or it is empty)");
        return 0;
    }
    std::lock_guard<std::mutex> lock(tapsMutex_);
    hook.handle = nextTap_++;
    hook.owner = owner;
    streamHooks_.push_back(std::move(hook));
    return streamHooks_.back().handle;
}

void Engine::removeTap(TapHandle handle) {
    std::lock_guard<std::mutex> lock(tapsMutex_);
    iqTaps_.erase(std::remove_if(iqTaps_.begin(), iqTaps_.end(), [&](const IqTap& t) { return t.handle == handle; }),
                  iqTaps_.end());
    audioTaps_.erase(
        std::remove_if(audioTaps_.begin(), audioTaps_.end(), [&](const AudioTap& t) { return t.handle == handle; }),
        audioTaps_.end());
    streamHooks_.erase(
        std::remove_if(streamHooks_.begin(), streamHooks_.end(), [&](const StreamHook& h) { return h.handle == handle; }),
        streamHooks_.end());
}

void Engine::removeTaps(OwnerId owner) {
    {
        std::lock_guard<std::mutex> lock(tapsMutex_);
        iqTaps_.erase(std::remove_if(iqTaps_.begin(), iqTaps_.end(), [&](const IqTap& t) { return t.owner == owner; }),
                      iqTaps_.end());
        audioTaps_.erase(
            std::remove_if(audioTaps_.begin(), audioTaps_.end(), [&](const AudioTap& t) { return t.owner == owner; }),
            audioTaps_.end());
        streamHooks_.erase(
            std::remove_if(streamHooks_.begin(), streamHooks_.end(), [&](const StreamHook& h) { return h.owner == owner; }),
            streamHooks_.end());
    }
    setBypassDemodulation(owner, false);
}

void Engine::setBypassDemodulation(OwnerId owner, bool bypass) {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    if (bypass) bypassOwners_.insert(owner);
    else bypassOwners_.erase(owner);
    receiver_.setBypassDemodulation(!bypassOwners_.empty());
}

bool Engine::bypassDemodulation() const {
    std::lock_guard<std::recursive_mutex> lock(chainMutex_);
    return !bypassOwners_.empty();
}

// ---- DSP thread -------------------------------------------------------------------

void Engine::dspLoop() {
    using clock = std::chrono::steady_clock;
    while (!quit_) {
        const double rate = rate_.load();
        const size_t minBlock = std::max<size_t>(256, size_t(rate / 100.0));
        {
            std::unique_lock<std::mutex> lock(dataMutex_);
            dataCv_.wait_for(lock, std::chrono::milliseconds(20),
                             [&] { return quit_.load() || iqRing_.readAvailable() >= minBlock; });
        }
        if (quit_) break;
        if (flushRequested_.exchange(false)) {
            iqRing_.skip(iqRing_.readAvailable());
            std::lock_guard<std::recursive_mutex> lock(chainMutex_);
            receiver_.reset();
            dcBlocker_.reset();
            continue;
        }
        const size_t want = std::clamp<size_t>(size_t(rate / 50.0), 2048, block_.size());
        const size_t n = iqRing_.read(block_.data(), want);
        if (n == 0) continue;

        const auto t0 = clock::now();
        processBlock(block_.data(), n);
        const double spent = std::chrono::duration<double>(clock::now() - t0).count();
        if (rate > 0.0) {
            const float load = float(spent / (double(n) / rate));
            dspLoad_ = dspLoad_.load() * 0.9f + load * 0.1f;
        }
    }
}

void Engine::processBlock(Complex* data, size_t count) {
    const double rate = rate_.load();
    const double center = center_.load();
    if (rate <= 0.0) return;

    if (swapIq_)
        for (size_t i = 0; i < count; i++) data[i] = Complex(data[i].imag(), data[i].real());

    if (dcBlock_) {
        if (dcRate_ != rate) {
            dcBlocker_.setRate(1.0 - std::exp(-kTwoPi * 5.0 / rate));
            dcRate_ = rate;
        }
        dcBlocker_.process(data, count);
    }
    if (iqCorrection_) iqBalancer_.process(data, count);  // measured on the spectrum, see computeSpectrumFrame()

    runStreamHooks(ProcessorType::RawIQ, data, count, rate, center);
    {
        std::lock_guard<std::mutex> lock(tapsMutex_);
        for (auto& t : iqTaps_) t.cb(data, count, rate, center);
    }

    processSpectrum(data, count, rate, center);

    audioBuf_.clear();
    {
        std::lock_guard<std::recursive_mutex> lock(chainMutex_);
        blockVfo_ = vfo_.load();
        receiver_.setInputRate(rate);
        receiver_.setAudioRate(audio_.sampleRate());
        receiver_.setOffset(blockVfo_ - center);
        receiver_.process(data, count, audioBuf_);  // runs the hooks from DecimatedAndFilteredIQ to FilteredAudioOutput
        signalLevel_ = receiver_.signalLevel();
        squelchOpen_ = receiver_.squelchOpen();
    }
    if (!audioBuf_.empty()) runStreamHooks(ProcessorType::MonitorAF, audioBuf_.data(), audioBuf_.size(), audio_.sampleRate(), 0.0);

    const float v = volume_.load();
    const float gain = muted_ ? 0.0f : v * v;
    for (auto& f : audioBuf_) f = Stereo(dsp::softClip(f.l * gain), dsp::softClip(f.r * gain));

    {
        std::lock_guard<std::mutex> lock(tapsMutex_);
        for (auto& t : audioTaps_) t.cb(audioBuf_.data(), audioBuf_.size(), audio_.sampleRate());
    }
    audio_.write(audioBuf_.data(), audioBuf_.size());
}

// ---- persistence ------------------------------------------------------------------

void Engine::loadState(const nlohmann::json& j) {
    if (!j.is_object()) return;
    vfo_ = j.value("frequency", vfo_.load());
    center_ = j.value("center", center_.load());
    centerTuning_ = j.value("centerTuning", centerTuning_);
    dcBlock_ = j.value("dcBlock", dcBlock_.load());
    swapIq_ = j.value("swapIq", swapIq_.load());
    iqCorrection_ = j.value("iqCorrection", iqCorrection_.load());
    volume_ = std::clamp(j.value("volume", volume_.load()), 0.0f, 1.0f);
    muted_ = j.value("muted", muted_.load());
    if (j.contains("bandwidths") && j["bandwidths"].is_object())
        for (auto& [k, v] : j["bandwidths"].items())
            if (v.is_number()) bandwidths_[k] = v.get<double>();
    if (j.contains("snaps") && j["snaps"].is_object())
        for (auto& [k, v] : j["snaps"].items())
            if (v.is_number()) snaps_[k] = v.get<double>();
    if (j.contains("modeStates") && j["modeStates"].is_object()) modeStates_ = j["modeStates"];
    if (j.contains("audioFilters") && j["audioFilters"].is_object()) {
        for (auto& [mode, v] : j["audioFilters"].items()) {
            AudioFilterSettings f = audioFiltersFor(mode);
            f.highpass = v.value("highpass", f.highpass);
            f.highpassHz = v.value("highpassHz", f.highpassHz);
            f.lowpass = v.value("lowpass", f.lowpass);
            f.lowpassHz = v.value("lowpassHz", f.lowpassHz);
            f.noiseReduction = v.value("noiseReduction", f.noiseReduction);
            f.noiseReductionDb = std::clamp(v.value("noiseReductionDb", f.noiseReductionDb), 3.0f, 30.0f);
            audioFilters_[mode] = f;
        }
    }
    setSquelch(j.value("squelch", squelchEnabled_), j.value("squelchLevel", squelchLevel_));

    if (j.contains("spectrum")) {
        const auto& s = j["spectrum"];
        SpectrumSettings ss = spectrumSettings();
        ss.fftSize = s.value("fftSize", ss.fftSize);
        ss.window = dsp::WindowType(std::clamp(s.value("window", int(ss.window)), 0, dsp::kWindowTypeCount - 1));
        ss.frameRate = s.value("frameRate", ss.frameRate);
        setSpectrumSettings(ss);
    }

    preferredSource_ = j.value("source", preferredSource_);
    std::string m = j.value("mode", preferredMode_);
    {
        std::lock_guard<std::recursive_mutex> lock(chainMutex_);
        // Re-apply so that restored bandwidths / states take effect.
        for (auto& e : modes_) {
            if (e.instance) {
                auto it = modeStates_.find(e.name);
                if (it != modeStates_.end()) e.instance->loadState(*it);
            }
        }
        mode_.clear();
    }
    if (!setMode(m)) {
        // Probably provided by a plugin that is not loaded yet.
        setMode("WFM");
        std::lock_guard<std::recursive_mutex> lock(chainMutex_);
        preferredMode_ = m;
    }
    if (!preferredSource_.empty()) selectSource(preferredSource_);
}

void Engine::saveState(nlohmann::json& j) {
    j["frequency"] = vfo_.load();
    j["center"] = center_.load();
    j["centerTuning"] = centerTuning_;
    j["dcBlock"] = dcBlock_.load();
    j["swapIq"] = swapIq_.load();
    j["iqCorrection"] = iqCorrection_.load();
    j["volume"] = volume_.load();
    j["muted"] = muted_.load();
    j["source"] = current_ ? current_->name() : preferredSource_;
    {
        std::lock_guard<std::recursive_mutex> lock(chainMutex_);
        j["mode"] = mode_.empty() ? preferredMode_ : mode_;
        j["squelch"] = squelchEnabled_;
        j["squelchLevel"] = squelchLevel_;
        for (auto& e : modes_) {
            if (!e.instance) continue;
            nlohmann::json st = nlohmann::json::object();
            e.instance->saveState(st);
            modeStates_[e.name] = st;
        }
        j["modeStates"] = modeStates_;
        j["bandwidths"] = bandwidths_;
        j["snaps"] = snaps_;
        nlohmann::json filters = nlohmann::json::object();
        for (const auto& [mode, f] : audioFilters_)
            filters[mode] = {{"highpass", f.highpass},
                             {"highpassHz", f.highpassHz},
                             {"lowpass", f.lowpass},
                             {"lowpassHz", f.lowpassHz},
                             {"noiseReduction", f.noiseReduction},
                             {"noiseReductionDb", f.noiseReductionDb}};
        j["audioFilters"] = filters;
    }
    SpectrumSettings ss = spectrumSettings();
    j["spectrum"] = {{"fftSize", ss.fftSize}, {"window", int(ss.window)}, {"frameRate", ss.frameRate}};
}

}  // namespace SharkSDR
