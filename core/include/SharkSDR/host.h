#pragma once

#include <SharkSDR/demodulator.h>
#include <SharkSDR/log.h>
#include <SharkSDR/source.h>
#include <SharkSDR/types.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace SharkSDR {

using TapHandle = uint64_t;

// Called on the DSP thread with every block of raw IQ samples.
using IqTapCallback = std::function<void(const Complex* samples, size_t count,
                                         double sampleRate, double centerFrequency)>;

// Called on the DSP thread with every block of demodulated audio (after volume).
using AudioTapCallback = std::function<void(const Stereo* frames, size_t count, double sampleRate)>;

// Points of the receive chain where stream hooks run, in processing order.
// They are the stream hooks of SDR# (ProcessorType, with the same names), so
// a decoder written for SDR# maps onto them one to one:
//
//   RawIQ                   Complex  source rate   the whole tuner span (after Swap I/Q, DC removal, IQ correction)
//   DecimatedAndFilteredIQ  Complex  channel rate  the VFO channel after the channel filter, the VFO at 0 Hz
//   FMMPX                   float    250 kHz       WFM only: discriminator output, before stereo decoding and de-emphasis
//   DemodulatorOutput       Stereo   channel rate  demodulated audio (silent while the squelch is closed)
//   FilteredAudioOutput     Stereo   audio rate    after the audio low-pass, noise reduction and high/low-pass filters
//   MonitorAF               Stereo   audio rate    the audio being played, before mute and volume
//
// A hook may change the samples in place: the rest of the chain continues
// with what it leaves in the buffer. A digital voice decoder typically reads
// RawIQ or DecimatedAndFilteredIQ and writes its voice into MonitorAF,
// replacing or mixing the radio's audio, so that the volume and mute
// controls apply to it like to any other audio.
enum class ProcessorType {
    RawIQ,
    DecimatedAndFilteredIQ,
    FMMPX,
    DemodulatorOutput,
    FilteredAudioOutput,
    MonitorAF,
};

// Passed to stream hooks along with every block.
struct StreamInfo {
    ProcessorType type = ProcessorType::RawIQ;
    double sampleRate = 0.0;
    // IQ streams: the RF frequency at 0 Hz (the tuner center for RawIQ, the
    // VFO for DecimatedAndFilteredIQ). 0 for the other streams.
    double frequency = 0.0;
};

using IqProcessor = std::function<void(Complex* samples, size_t count, const StreamInfo& info)>;
using RealProcessor = std::function<void(float* samples, size_t count, const StreamInfo& info)>;
using AudioProcessor = std::function<void(Stereo* frames, size_t count, const StreamInfo& info)>;

// Passed to spectrum overlays to draw markers on top of the FFT plot.
struct SpectrumOverlayContext {
    ImDrawList* drawList = nullptr;
    ImVec2 plotMin;            // top-left of the FFT plot in screen coordinates
    ImVec2 plotMax;            // bottom-right of the FFT plot
    double viewStartHz = 0.0;  // absolute frequency at plotMin.x
    double viewEndHz = 0.0;    // absolute frequency at plotMax.x
    bool* clickConsumed = nullptr;  // set to true to stop the click from tuning the VFO

    float frequencyToX(double hz) const {
        return plotMin.x + float((hz - viewStartHz) / (viewEndHz - viewStartHz)) * (plotMax.x - plotMin.x);
    }
    double xToFrequency(float x) const {
        return viewStartHz + double(x - plotMin.x) / double(plotMax.x - plotMin.x) * (viewEndHz - viewStartHz);
    }
};

// The interface through which a plugin talks to the application. Each plugin
// gets its own Host instance: everything a plugin registers is tracked and
// removed automatically when the plugin is unloaded.
//
// Unless stated otherwise, call these methods from the UI thread (i.e. from
// Plugin::init(), panel/frame callbacks or other ImGui code).
class Host {
public:
    virtual ~Host() = default;

    // ---- Extension points -------------------------------------------------

    // Add an IQ source to the source selector.
    virtual void registerSource(std::shared_ptr<Source> source) = 0;

    // Add a demodulation mode (shown as a button in the "Radio" panel).
    virtual void registerDemodulator(const std::string& name, DemodulatorFactory factory) = 0;

    // Add a collapsible panel to the sidebar. Plugin panels start in the
    // right sidebar; the user can drag them by the title (or use its
    // right-click menu) to the left one or to another place. `draw` is called
    // every frame while the panel is open, inside the sidebar (use ImGui widgets).
    virtual void registerPanel(const std::string& title, std::function<void()> draw) = 0;

    // Called once per frame after the main window is drawn. Use it to show
    // your own ImGui windows.
    virtual void registerFrameCallback(std::function<void()> callback) = 0;

    // Add entries to the "Tools" menu. `draw` is called inside the menu and
    // should use ImGui::MenuItem().
    virtual void registerMenu(std::function<void()> draw) = 0;

    // Draw on top of the spectrum plot (e.g. bookmark markers).
    virtual void registerSpectrumOverlay(std::function<void(const SpectrumOverlayContext&)> draw) = 0;

    // Receive raw IQ / audio data. The callbacks run on the DSP thread: keep
    // them short and never block. Thread-safe.
    virtual TapHandle addIqTap(IqTapCallback callback) = 0;
    virtual TapHandle addAudioTap(AudioTapCallback callback) = 0;
    virtual void removeTap(TapHandle handle) = 0;

    // Stream hooks, as SDR#'s RegisterStreamHook() (see ProcessorType). The
    // hook's sample type must match the point: IqProcessor for the IQ points,
    // RealProcessor for FMMPX, AudioProcessor for the audio points; otherwise
    // nothing is registered and 0 is returned. Hooks at the same point run in
    // the order they were registered. They run on the DSP thread: keep them
    // short, never block, and do not register or remove hooks from inside
    // one. Thread-safe.
    virtual TapHandle registerStreamHook(ProcessorType type, IqProcessor hook) = 0;
    virtual TapHandle registerStreamHook(ProcessorType type, RealProcessor hook) = 0;
    virtual TapHandle registerStreamHook(ProcessorType type, AudioProcessor hook) = 0;
    // Once this returns, the hook is not called again.
    void unregisterStreamHook(TapHandle handle) { removeTap(handle); }

    // Leaves the built-in demodulator out (SDR#'s BypassDemodulation): the
    // audio points then carry silence, for decoders that write all the audio
    // into MonitorAF themselves. Released automatically when the plugin is
    // unloaded.
    virtual void setBypassDemodulation(bool bypass) = 0;
    virtual bool bypassDemodulation() const = 0;  // set by any plugin

    // ---- Receiver control -------------------------------------------------

    virtual bool isRunning() const = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual std::string sourceName() const = 0;
    virtual bool sourceTunable() const = 0;       // false for e.g. file playback
    virtual double sampleRate() const = 0;        // current IQ rate
    virtual double centerFrequency() const = 0;   // frequency of the IQ stream center
    // Retunes the source (SDR#'s CenterFrequency); the VFO is kept inside the band.
    virtual void setCenterFrequency(double hz) = 0;

    virtual double frequency() const = 0;         // VFO (listening) frequency
    virtual void setFrequency(double hz) = 0;

    virtual std::string mode() const = 0;
    virtual std::vector<std::string> modes() const = 0;
    virtual bool setMode(const std::string& name) = 0;

    virtual double bandwidth() const = 0;
    virtual void setBandwidth(double hz) = 0;

    virtual float signalLevel() const = 0;        // channel power in dBFS
    virtual bool squelchOpen() const = 0;

    // ---- Configuration and utilities --------------------------------------

    // The plugin's own persistent settings (saved as JSON on exit).
    virtual nlohmann::json& config() = 0;
    virtual void saveConfig() = 0;

    // Default folder for files created by plugins (recordings, ...).
    virtual std::string dataDirectory() const = 0;

    virtual void log(LogLevel level, const std::string& message) = 0;
};

}  // namespace SharkSDR
