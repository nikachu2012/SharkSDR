# SharkSDR

English | [日本語](README.ja.md)

[![build](https://github.com/nikachu2012/SharkSDR/actions/workflows/build.yml/badge.svg)](https://github.com/nikachu2012/SharkSDR/actions/workflows/build.yml)

A cross-platform (Windows / Linux / macOS) SDR receiver written in C++17 with [Dear ImGui](https://github.com/ocornut/imgui).
It uses the familiar layout of SDR++ and SDR# (sidebar on the left, spectrum and waterfall on the right, a large frequency display on top),
and sources, demodulation modes, panels and tools can all be added later as **plugins (shared libraries)**.

![screenshot](docs/screenshot.png)

## Features

| Area | Features |
|---|---|
| Display | FFT spectrum (smoothing, peak hold, fill), waterfall (Turbo / Classic / Viridis / Inferno / Grayscale), zoom and pan, dB range control, auto range |
| Tuning | Frequency dial (change each digit with the wheel or a click), click/drag tuning on the spectrum, wheel step tuning, drag the band edges to change the bandwidth, center tuning, direct entry (`145.5M` etc.) |
| Demodulation | WFM (stereo decoder with automatic stereo/mono blend based on signal quality, 50/75 µs de-emphasis), NFM, AM, USB, LSB, DSB, CW (adjustable BFO), RAW, plus SAM (synchronous AM) as a plugin |
| Receive chain | NCO → cascaded half-band decimation by 2 → polyphase resampler → channel filter → squelch → demodulator → audio LPF/resampling, AGC, DC removal, I/Q swap |
| Noise reduction | IF noise reduction for FM (carrier-tracking, +7 to 9 dB SINAD on weak stations), per-mode high-pass/low-pass filters (8th-order Butterworth, for removing CTCSS tones or reducing FM hiss) |
| Audio | Output through miniaudio (WASAPI / CoreAudio / ALSA / PulseAudio, etc.), device selection, volume and mute |
| Sources (plugins) | Signal generator (a virtual band for demos), IQ file playback (WAV / cu8 / cs8 / cs16 / cf32), rtl_tcp, SoapySDR\*, native RTL-SDR\* |
| Tools (plugins) | Recorder (audio or baseband IQ to WAV), bookmarks (markers on the spectrum), Hello Example as a template |
| Other | Settings saved automatically (JSON), plugin manager (enable/disable while running), log window, dark/light theme, UI scale, automatic merging of a Japanese font |

\* Built only when SoapySDR / librtlsdr is found in the build environment.

## Building

Requirements: CMake 3.20 or newer, a C++17 compiler and Git. Dependencies are fetched automatically by CMake's FetchContent: GLFW 3.4, Dear ImGui 1.92.9b, miniaudio 0.11.25 and nlohmann/json 3.12.0.

### Windows (Visual Studio 2022)

From the "x64 Native Tools Command Prompt for VS 2022":

```bat
cmake --preset release
cmake --build --preset release
build\bin\SharkSDR.exe
```

Use `cmake --preset vs2022` if you want a Visual Studio solution.

### Linux (Ubuntu / Debian)

```sh
sudo apt install build-essential cmake ninja-build libgl1-mesa-dev xorg-dev \
                 libwayland-dev libxkbcommon-dev wayland-protocols
# Optional: for the hardware plugins
sudo apt install libsoapysdr-dev librtlsdr-dev

cmake --preset release
cmake --build --preset release
./build/bin/SharkSDR
```

If you would rather not install the Wayland development packages, add `-DSHARKSDR_GLFW_WAYLAND=OFF` to build for X11 only.

### macOS

```sh
brew install cmake ninja
# Optional: brew install soapysdr librtlsdr
cmake --preset release
cmake --build --preset release
./build/bin/SharkSDR
```

Everything ends up in `build/bin/`; copy the whole folder and it runs anywhere.

```
build/bin/
├── SharkSDR(.exe)            the application (a thin wrapper)
├── SharkSDR_core(.dll/.so)   core library (ImGui, DSP, plugin API, UI)
├── plugins/                  plugins (loaded automatically at startup)
└── resources/fonts/          fonts
```

### Tests

Headless tests check the DSP chain (the output tone of every demodulation mode, WFM stereo separation, SSB opposite-sideband rejection, squelch, WAV reading and writing, and more).

```sh
ctest --preset release
```

There are also self-tests for developers that drive the GUI (they inject input into ImGui instead of using the real mouse):

```sh
# Checks click tuning, wheel tuning, Ctrl+wheel zoom, band edge dragging, right-drag panning
# and typing values into sliders (Enter applies / Esc cancels, vertical sliders too),
# and records for about 1 second with Ctrl+R meanwhile (exit code 2 on failure)
SharkSDR --config <temporary folder> --autostart --test-ui --screenshot ui.png --frames 220
# Unloads and reloads every plugin while receiving
SharkSDR --config <temporary folder> --autostart --test-plugin-reload --screenshot reload.png --frames 600
```

## Usage

On first start the **Signal Generator** source is selected (a virtual band around 100 MHz with WFM stereo, NFM, AM, USB, LSB and CW stations),
so you can try it right away without hardware: press the ▶ button (or Space). Click a frequency in the station list in the sidebar to tune to that station.

| Action | Result |
|---|---|
| Click or drag on the spectrum / waterfall | Move the VFO |
| Drag an edge of the VFO band on the spectrum | Change the bandwidth |
| Mouse wheel on the spectrum | Tune one step |
| Ctrl + wheel | Zoom around the cursor |
| Right or middle drag | Pan the zoomed view |
| Wheel over a digit of the frequency dial / click its upper or lower half | Increase or decrease that digit (right-click sets the lower digits to 0) |
| Double-click a slider (or Ctrl+click) | Type a value (Enter applies, Esc cancels; units such as `12.5k` or `-40 dB` are accepted) |
| Space / ← → / M | Start or stop / step tuning / mute |
| Ctrl+S / Ctrl+Q | Save settings / quit |
| Ctrl+R | Start or stop recording (Recorder plugin) |

On macOS, use Cmd instead of Ctrl.

### Hardware

- **rtl_tcp** (all platforms, no driver to link): run `rtl_tcp -a 0.0.0.0` on the computer with the RTL-SDR, then choose "RTL-TCP" as the source and enter the host name.
- **SoapySDR**: install SoapySDR and the module for your device (SoapyRTLSDR, SoapyHackRF, SoapyAirspy, SoapySDRPlay3, etc.) before building to get the "SoapySDR" source.
  On Windows, set `CMAKE_PREFIX_PATH` to a SoapySDR installed with [PothosSDR](https://downloads.myriadrf.org/builds/PothosSDR/) or similar.
- **Native RTL-SDR**: the "RTL-SDR" source is built when librtlsdr is found.

Vendor DLLs (such as `SoapySDR.dll`) are also found when placed in the `plugins/` folder next to the plugins.

### Command line options

```
--config DIR       settings folder (default: the per-user settings folder)
--plugins DIR      an additional plugin folder
--no-plugins       do not load plugins
--autostart        start receiving right after launch
--screenshot FILE  save the window as PNG after --frames N frames and quit (for docs and testing)
```

Settings are stored in `%APPDATA%\SharkSDR` on Windows, `~/Library/Application Support/SharkSDR` on macOS and `~/.config/SharkSDR` on Linux.
Put a file named `portable` next to the executable to switch to portable mode, which stores settings in that folder.

## Extending with plugins

The extension points are sources, demodulation modes, sidebar panels, the Tools menu, custom windows, overlays on the spectrum, and IQ/audio taps.
This is all a minimal plugin needs:

```cpp
#include <SharkSDR/plugin.h>

class MyPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        host.registerPanel("My panel", [&host] {
            ImGui::Text("VFO: %.0f Hz", host.frequency());
            if (ImGui::Button("Go to 145 MHz")) host.setFrequency(145e6);
        });
    }
};

SHARKSDR_PLUGIN(MyPlugin, "My plugin", "0.1.0", "Me", "Example")
```

```cmake
sharksdr_add_plugin(my_plugin SOURCES my_plugin.cpp)
```

Plugins kept outside the repository can be built along with it using `-DSHARKSDR_EXTRA_PLUGINS=/path/to/my_plugin`.
See **[docs/PLUGIN_GUIDE.md](docs/PLUGIN_GUIDE.md)** for details. The bundled plugins (`plugins/`) also work as samples.

## Source layout

```
core/include/SharkSDR/   public API (plugin.h, host.h, source.h, demodulator.h, dsp/*, ui/*, util/*)
core/src/dsp/            FFT, FIR design, resamplers, receive chain, built-in demodulators
core/src/ui/             theme and widgets (frequency dial, file picker, etc.)
core/src/app/            the application (engine/DSP thread, spectrum view, plugin manager, layout)
app/                     main()
plugins/                 bundled plugins
tests/                   DSP tests
```

Threads: UI thread (ImGui) / source streaming thread → lock-free ring buffer → DSP thread (FFT, receive chain, taps) → audio callback.

## Testing status

- Windows 11 + MSVC 19.44 (VS 2022): Release and Debug builds (no warnings, no ImGui asserts in Debug), all DSP tests and the UI self-tests pass. Reception with the signal generator, IQ file playback and rtl_tcp (against a simulated server), unloading and reloading plugins while running, and saving and restoring settings have been checked. The SoapySDR / RTL-SDR plugins have been built and loaded against the libraries, but not tested with real devices. Audio output has been checked through the buffer state, not by ear.
- Linux / macOS: written with portability in mind. CI (`.github/workflows/build.yml`) builds and runs the DSP tests on Windows, Linux and macOS; the GUI itself has not been tried on Linux or macOS yet.

## Third-party

Dear ImGui (MIT), GLFW (zlib), miniaudio (MIT-0 / Public Domain), nlohmann/json (MIT), Roboto / Cousine fonts (Apache 2.0).

## License

[MIT License](LICENSE). The third-party libraries and fonts are covered by their own licenses.
