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

Either way, the output goes to `build/bin/plugins/`.

**Inside the repository**: copy `plugins/hello_example`, rename the folder and the target, and add it to `SHARKSDR_BUNDLED_PLUGINS` in `plugins/CMakeLists.txt`.

**Outside the repository**: add it to the build without touching the application's sources.

```sh
cmake --preset release -DSHARKSDR_EXTRA_PLUGINS="/path/to/my_plugin;/path/to/another"
cmake --build --preset release
```

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

The Host also controls the receiver (`start/stop`, `frequency/setFrequency`, `mode/setMode`, `bandwidth/setBandwidth`, `signalLevel`, `sampleRate`, `centerFrequency`, etc.),
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

## 6. Threading rules

| Called on | What |
|---|---|
| UI thread | `init` / `shutdown`, panels, menus, frame callbacks, overlays, `Source::start/stop/tune/drawSettings`, almost every Host method |
| DSP thread | IQ and audio taps, `Demodulator::process` |
| The source's own thread | `SampleSink::pushSamples` / `sourceStopped` |

Keep tap callbacks short and never block in them (move disk writes and the like to another thread; see `plugins/recorder`).
Once `removeTap()` returns, the callback is guaranteed not to be called again.

## 7. Building blocks you can use

Plugins can use these parts of `SharkSDR_core` directly.

- **DSP** (`SharkSDR/dsp/*.h`): `Fft`, window functions, `designLowpass` / `designComplexBandpass` (Kaiser-window FIR design), `FirFilter`, `ComplexFirFilter`, `FirDecimator`, `PowerDecimator`,
  `Resampler<T>` (polyphase resampler with any ratio), `Nco`, `Agc`, `RingBuffer`, `OnePole`, `DcBlocker`,
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

## 8. Bundled plugins (as samples)

| Folder | What it is | Worth a look for |
|---|---|---|
| `hello_example` | Panel, menu, custom window, audio tap | A template |
| `signal_generator` | Signal generator source with a virtual band | Implementing `Source`, modulation on the transmit side |
| `file_source` | IQ file playback | Non-tunable source, `sourceStopped`, `PathPicker` |
| `rtl_tcp_source` | rtl_tcp client | `TcpSocket`, changing settings while running |
| `soapy_source` | SoapySDR | Depending on an external library (`LIBRARIES`) |
| `rtlsdr_source` | librtlsdr | A driver with asynchronous callbacks |
| `am_sync_demod` | Synchronous AM (SAM) | Implementing `Demodulator`, saving state |
| `recorder` | Recording | Taps, a writer thread |
| `bookmarks` | Bookmarks | Spectrum overlay, saving settings |
