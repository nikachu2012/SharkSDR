# SharkSDR plugin guide

English | [日本語](PLUGIN_GUIDE.ja.md)

A SharkSDR plugin is a shared library (`.dll` / `.so` / `.dylib`) in a `plugins/` folder.
Plugins are loaded automatically at startup and can be enabled or disabled while running from **Plugins → Plugin manager**.

Plugins link against the same `SharkSDR_core` library as the application, so they share **the same ImGui context** and can use the same DSP blocks and widgets directly.

## 1. A minimal plugin

```cpp
// my_plugin.cpp
#include <SharkSDR/plugin.h>

class MyPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        host.registerPanel("My panel", [&host] {
            ImGui::Text("VFO: %.0f Hz", host.frequency());
        });
    }
};

//              class     name         version  author  description
SHARKSDR_PLUGIN(MyPlugin, "My plugin", "0.1.0", "Me", "Shows the VFO frequency")
```

```cmake
# CMakeLists.txt
sharksdr_add_plugin(my_plugin SOURCES my_plugin.cpp)
```

The `SHARKSDR_PLUGIN` macro defines the C entry points the application looks for (API version, build signature, information, create and destroy).

## 2. Building

Built along with SharkSDR (the first two ways), the output goes to `build/bin/plugins/`.

**Inside the repository**: copy `plugins/hello_example`, rename the folder and the target, and add it to `SHARKSDR_BUNDLED_PLUGINS` in `plugins/CMakeLists.txt`.

**Outside the repository**: add it to the build without touching the application's sources.

```sh
cmake --preset release -DSHARKSDR_EXTRA_PLUGINS="/path/to/my_plugin;/path/to/another"
cmake --build --preset release
```

**On its own, as a file to drop in**: every SharkSDR build folder is also a plugin SDK (`SharkSDRConfig.cmake`).
A plugin project builds against it without rebuilding SharkSDR, and the resulting file only has to be copied into that build's `bin/plugins/`.
Write the plugin's `CMakeLists.txt` so that it works both ways:

```cmake
cmake_minimum_required(VERSION 3.20)
if(NOT COMMAND sharksdr_add_plugin)  # not inside SharkSDR's build
    project(my_plugin LANGUAGES CXX)
    find_package(SharkSDR CONFIG REQUIRED PATHS "${SHARKSDR_BUILD_DIR}" NO_DEFAULT_PATH)
endif()
sharksdr_add_plugin(my_plugin SOURCES my_plugin.cpp)
```

```sh
cmake -S my_plugin -B my_plugin/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHARKSDR_BUILD_DIR=/path/to/SharkSDR/build
cmake --build my_plugin/build      # -> my_plugin/build/plugins/my_plugin.(dll|so|dylib)
```

Use the same compiler and configuration (Release / Debug) as the SharkSDR build. When SharkSDR's plugin API changes, build the plugin again: the plugin manager tells why an old one does not load.

A finished plugin file is loaded from the `plugins/` folder next to the executable, the `plugins/` folder in the user settings folder, or a folder given with `--plugins DIR`.

> **About the ABI**: plugins exchange C++ objects (`std::function`, `std::shared_ptr`, `nlohmann::json`, etc.) with the application,
> so they must be built with the same compiler family and the same ImGui version as the application (and, with MSVC, the same Debug/Release configuration).
> If they do not match, the plugin manager shows the reason and the plugin is not loaded.

## 3. Extension points

Register things through the `SharkSDR::Host` passed to `init()` ([`core/include/SharkSDR/host.h`](../core/include/SharkSDR/host.h)).
Everything you register is **removed automatically when the plugin is unloaded** (`shutdown()` is called right after that).

| Method | Purpose | Examples |
|---|---|---|
| `registerSource(std::shared_ptr<Source>)` | IQ source (hardware, network, file…) | `signal_generator`, `rtl_tcp_source`, `soapy_source` |
| `registerDemodulator(name, factory)` | Demodulation mode (adds a button to the Radio panel) | `am_sync_demod` |
| `registerPanel(title, draw)` | Collapsible sidebar panel | `recorder`, `bookmarks` |
| `registerMenu(draw)` | Items in the Tools menu | `bookmarks`, `hello_example` |
| `registerFrameCallback(fn)` | Called every frame (for your own windows) | `hello_example` |
| `registerSpectrumOverlay(draw)` | Drawing and click handling on the spectrum | `bookmarks` |
| `addIqTap(cb)` / `addAudioTap(cb)` | Access to IQ and demodulated audio (called on the DSP thread) | `recorder` |
| `registerStreamHook(type, hook)` | Read or change the signal at a point of the receive chain, like SDR#'s stream hooks (see 6) | `hello_example` |
| `setBypassDemodulation(bool)` | Leave the built-in demodulator out (SDR#'s BypassDemodulation) | |

The Host also controls the receiver (`start/stop`, `frequency/setFrequency`, `centerFrequency/setCenterFrequency`, `mode/setMode`, `bandwidth/setBandwidth`, `signalLevel`, `sampleRate`, `sourceTunable`, etc.),
and provides per-plugin settings with `config()` (JSON, saved automatically on exit), `log()` and `dataDirectory()`.

## 4. Writing a source

Implement `SharkSDR::Source` from [`core/include/SharkSDR/source.h`](../core/include/SharkSDR/source.h).

```cpp
class MySource : public SharkSDR::Source {
public:
    std::string name() const override { return "My SDR"; }

    bool start(SharkSDR::SampleSink& sink) override {
        quit_ = false;
        thread_ = std::thread([this, &sink] {
            std::vector<SharkSDR::Complex> buf(16384);
            while (!quit_) {
                // ... read from the device and convert to complex samples, amplitude 1.0 = full scale ...
                sink.pushSamples(buf.data(), buf.size());
            }
        });
        return true;              // on failure return false and give the reason in lastError()
    }
    void stop() override {         // streaming must have stopped completely before this returns
        quit_ = true;
        if (thread_.joinable()) thread_.join();
    }
    double sampleRate() const override { return rate_; }      // called from any thread
    void tune(double hz) override { /* retune the hardware */ }
    void drawSettings(bool running) override { /* settings UI with ImGui */ }

private:
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<double> rate_{2.4e6};
};
```

- When the stream ends by itself (end of file, disconnection, etc.), call `sink.sourceStopped("reason")`. The application then calls `stop()` on the UI thread.
- Sources that cannot change frequency (such as file playback) return `false` from `tunable()` and report their center frequency from `frequency()`.
- The sample rate may change while running (the DSP follows when the value of `sampleRate()` changes).

## 5. Writing a demodulation mode

Implement `SharkSDR::Demodulator` from [`core/include/SharkSDR/demodulator.h`](../core/include/SharkSDR/demodulator.h).
The application takes care of frequency shifting, decimation, resampling, the channel filter, squelch, and the audio LPF and resampling,
so you only write **the demodulation itself**.

```cpp
class MyDemod : public SharkSDR::Demodulator {
public:
    double channelRate() const override { return 24000.0; }   // rate of the input IQ (the output audio uses the same rate)
    double defaultBandwidth() const override { return 6000.0; }
    SharkSDR::BandShape bandShape() const override { return SharkSDR::BandShape::Center; }
    double audioCutoff() const override { return 3000.0; }     // the application cuts everything above this

    void process(const SharkSDR::Complex* in, size_t n, SharkSDR::Stereo* out) override {
        for (size_t i = 0; i < n; i++) out[i] = SharkSDR::Stereo(std::abs(in[i]));
    }
};

host.registerDemodulator("MYMODE", [] { return std::make_unique<MyDemod>(); });
```

Mode-specific settings go in `drawSettings()` (shown in the Radio panel) and are saved with `loadState()` / `saveState()`.
The DSP thread is paused during these calls, so no locking is needed.

## 6. Writing a decoder: stream hooks (as in SDR#)

A decoder for a digital mode needs the signal from some point of the receive chain, and a voice decoder also plays its own audio.
SharkSDR handles this the same way as SDR#: register a *stream hook* at one of the points below ([`host.h`](../core/include/SharkSDR/host.h), `ProcessorType`, with SDR#'s names).
A hook gets every block in place and may change it; the rest of the chain continues with what it leaves in the buffer.

| `ProcessorType` | Data | Rate | What it carries |
|---|---|---|---|
| `RawIQ` | `Complex` | source rate | The whole tuner span (after Swap I/Q, DC removal and IQ correction). `info.frequency` is the tuner center |
| `DecimatedAndFilteredIQ` | `Complex` | channel rate | The VFO channel after the channel filter, with the VFO at 0 Hz. `info.frequency` is the VFO |
| `FMMPX` | `float` | 250 kHz | WFM only: the discriminator output, before stereo decoding and de-emphasis |
| `DemodulatorOutput` | `Stereo` | channel rate | The demodulated audio (silent while the squelch is closed) |
| `FilteredAudioOutput` | `Stereo` | audio rate | After the audio low-pass, noise reduction and the high/low-pass filters |
| `MonitorAF` | `Stereo` | audio rate | The audio being played, before mute and volume |

The channel rate is the demodulator's `channelRate()` (WFM 250 kHz, NFM 50 kHz, AM/SSB 24 kHz); take the rate of every block from `info.sampleRate`.
Hooks at the same point run in the order they were registered. Registering a hook whose sample type does not match the point returns 0.

A voice decoder that cuts its channel out of the wideband IQ itself (so the Radio panel's bandwidth does not matter), and plays its voice instead of the radio while it has some:

```cpp
void init(SharkSDR::Host& host) override {
    host_ = &host;
    host.registerFrameCallback([this] { vfo_ = host_->frequency(); });  // the VFO, for the DSP thread

    // Signal in: the whole tuner span and its center frequency.
    host.registerStreamHook(SharkSDR::ProcessorType::RawIQ,
                            [this](SharkSDR::Complex* iq, size_t n, const SharkSDR::StreamInfo& info) {
                                decoder_.push(iq, n, info.sampleRate, vfo_ - info.frequency);
                            });
    // Voice out: replaces what is played while there is voice. Mute and volume still apply.
    host.registerStreamHook(SharkSDR::ProcessorType::MonitorAF,
                            [this](SharkSDR::Stereo* audio, size_t n, const SharkSDR::StreamInfo& info) {
                                if (decoder_.playing()) decoder_.readVoice(audio, n, info.sampleRate);
                            });
}
```

(`decoder_` stands for your decoder; `vfo_` is a `std::atomic<double>`.) To mix the voice into the radio's audio instead, add to `audio[i]` rather than overwriting it.
A decoder that makes all the audio itself can call `setBypassDemodulation(true)`: the demodulator is then left out, and the audio points carry silence for it to fill.
Hooks are removed, and the bypass released, automatically when the plugin is unloaded.

Coming from an SDR# plugin:

| SDR# | SharkSDR |
|---|---|
| `control.RegisterStreamHook(hook, ProcessorType.RawIQ)` | `host.registerStreamHook(ProcessorType::RawIQ, hook)` (returns a handle) |
| `control.UnregisterStreamHook(hook)` | `host.unregisterStreamHook(handle)` |
| `IIQProcessor.Process(Complex*, int length)` | `IqProcessor`: `(Complex*, size_t count, const StreamInfo&)` |
| `IRealProcessor.Process(float*, int length)` for FMMPX | `RealProcessor`: `(float*, size_t count, const StreamInfo&)` |
| `IRealProcessor.Process(float*, int length)` for audio (interleaved stereo, `length` floats) | `AudioProcessor`: `(Stereo*, size_t count, const StreamInfo&)` (`count` frames); `DemodulatorOutput` is stereo too |
| The `SampleRate` property | `info.sampleRate`, with every block |
| `Enabled` | Register and unregister the hook |
| `control.Frequency` / `control.CenterFrequency` | `host.frequency()` (UI thread) / `info.frequency` of the same block |
| Setting `control.Frequency` / `control.CenterFrequency` | `host.setFrequency()` / `host.setCenterFrequency()` (UI thread; a worker thread hands the call to a frame callback) |
| `control.SourceIsTunable` | `host.sourceTunable()` |
| `control.BypassDemodulation` | `host.setBypassDemodulation()` |

## 7. Threading rules

| Called on | What |
|---|---|
| UI thread | `init` / `shutdown`, panels, menus, frame callbacks, overlays, `Source::start/stop/tune/drawSettings`, almost every Host method |
| DSP thread | IQ and audio taps, stream hooks, `Demodulator::process` |
| The source's own thread | `SampleSink::pushSamples` / `sourceStopped` |

Keep tap and hook callbacks short and never block in them (move disk writes and the like to another thread; see `plugins/recorder`), and do not register or remove hooks from inside one.
Once `removeTap()` / `unregisterStreamHook()` returns, the callback is guaranteed not to be called again.

## 8. Building blocks you can use

Plugins can use these parts of `SharkSDR_core` directly.

- **DSP** (`SharkSDR/dsp/*.h`): `Fft`, window functions, `designLowpass` / `designComplexBandpass` (Kaiser-window FIR design), `FirFilter`, `ComplexFirFilter`, `FirDecimator`,
  `HalfBandDecimator` / `HalfBandInterpolator` / `DecimatorCascade` (`PowerDecimator`), `Resampler<T>` (polyphase resampler with any ratio),
  the SIMD dot products behind all of them (`dotProduct`, `pairDotProduct`), `Nco`, `Agc`, `RingBuffer`, `OnePole`, `DcBlocker`, `IqBalancer`, `accurateAtan2`,
  noise reduction (`FmIfNoiseReduction`, `AudioNoiseReduction`, `ButterworthFilter`),
  `Receiver` (the whole receive chain from frequency shift to audio; combine it with an IQ tap to make **an additional VFO**),
  and creating built-in demodulators with `createBuiltinDemodulator("NFM")` and the like
- **UI** (`SharkSDR/ui/*.h`): `FrequencyDial`, `InputFrequency`, `PathPicker` (file/folder picker dialog), `LevelMeter`, `ToggleButton`, `HelpMarker`,
  formatting functions such as `formatFrequency`, `Texture` (a GPU texture for ImGui::Image), `fonts()`
- **Layout**: helpers that line your rows up with the application's sidebar.
  - `LabeledRow("Gain")`: label on the left, widget on the right (the label column has the same width in every panel)
  - `CheckboxRow("Squelch", &on)`: a row whose label has a checkbox in front of it
  - `SameLineColumn()`: puts checkboxes or radio buttons in two columns
- **Sliders**: `ui::SliderFloat` / `ui::SliderInt` / `ui::VSliderFloat` take the same arguments as the ImGui functions of the same name.
  Double-click (or Ctrl+click) one to type a value (units such as `12.5k` or `-40 dB` are accepted, and the value is clamped to the range).
  Add `ImGuiSliderFlags_NoInput` to sliders where typing a value makes no sense (such as a playback position).

```cpp
SharkSDR::ui::LabeledRow("Gain");
if (SharkSDR::ui::SliderFloat("##gain", &gainDb, 0.0f, 49.6f, "%.1f dB")) applyGain();
ImGui::Checkbox("Bias-T", &biasTee);
SharkSDR::ui::SameLineColumn();
ImGui::Checkbox("Offset", &offset);
```

- **Utilities**: `WavWriter` / `WavReader` (`SharkSDR/util/wav.h`), `TcpSocket` (`SharkSDR/util/net.h`), path functions (`SharkSDR/paths.h`), logging (`SharkSDR/log.h`)

## 9. Bundled plugins (as samples)

| Folder | What it is | Worth a look for |
|---|---|---|
| `hello_example` | Panel, menu, custom window, stream hook | A template |
| `signal_generator` | Signal generator source with a virtual band | Implementing `Source`, modulation on the transmit side |
| `file_source` | IQ file playback | Non-tunable source, `sourceStopped`, `PathPicker` |
| `rtl_tcp_source` | rtl_tcp client | `TcpSocket`, changing settings while running |
| `soapy_source` | SoapySDR | Depending on an external library (`LIBRARIES`) |
| `rtlsdr_source` | librtlsdr | A driver with asynchronous callbacks |
| `am_sync_demod` | Synchronous AM (SAM) | Implementing `Demodulator`, saving state |
| `recorder` | Recording | Taps, a writer thread |
| `bookmarks` | Bookmarks | Spectrum overlay, saving settings |
