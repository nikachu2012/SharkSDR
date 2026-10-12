#pragma once

#include <SharkSDR/types.h>

#include <string>

namespace SharkSDR {

// Receives IQ samples from a Source. Implemented by the application.
class SampleSink {
public:
    virtual ~SampleSink() = default;

    // Hand over `count` samples. Thread-safe; usually called from the source's
    // own streaming thread. The data is copied before the call returns.
    virtual void pushSamples(const Complex* samples, size_t count) = 0;

    // Tell the application that streaming ended on its own (end of file,
    // connection lost, device unplugged...). Thread-safe. The application will
    // call Source::stop() afterwards from the UI thread.
    virtual void sourceStopped(const std::string& reason) = 0;
};

// An IQ sample source (SDR hardware, network stream, file, generator...).
// Sources are provided by plugins through Host::registerSource().
//
// Threading: start(), stop(), tune() and drawSettings() are called from the UI
// thread. sampleRate() and frequency() may be called from any thread.
class Source {
public:
    virtual ~Source() = default;

    // Name shown in the source selector, e.g. "RTL-TCP".
    virtual std::string name() const = 0;

    // Called when the user selects this source (e.g. to refresh device lists).
    virtual void onSelected() {}

    // Start streaming into `sink`. Return false on failure (see lastError()).
    virtual bool start(SampleSink& sink) = 0;

    // Stop streaming. Must not return before the source stopped calling the sink.
    virtual void stop() = 0;

    // Current sample rate in samples per second. May change while running.
    virtual double sampleRate() const = 0;

    // Whether the center frequency can be changed (false for e.g. file playback).
    virtual bool tunable() const { return true; }

    // Retune the hardware to `hz`. Only called when tunable() is true. May be
    // called both while stopped and while running.
    virtual void tune(double hz) = 0;

    // Center frequency of the stream for sources that are not tunable.
    virtual double frequency() const { return 0.0; }

    // Draw the source settings with ImGui (shown in the "Source" panel).
    virtual void drawSettings(bool running) = 0;

    // Human readable description of the last start() failure.
    virtual std::string lastError() const { return {}; }
};

}  // namespace SharkSDR
