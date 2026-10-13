#include "app/app.h"

#include "app/plugin_manager.h"
#include "app/png_writer.h"

#include <SharkSDR/application.h>
#include <SharkSDR/log.h>
#include <SharkSDR/paths.h>
#include <SharkSDR/ui/widgets.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#if defined(__APPLE__)
#  define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

// ImGui maps the Command key to "Ctrl" on macOS, so shortcuts read Cmd there.
#if defined(__APPLE__)
#  define SHARKSDR_CTRL_KEY "Cmd"
#else
#  define SHARKSDR_CTRL_KEY "Ctrl"
#endif

namespace SharkSDR {

const char* versionString() { return SHARKSDR_VERSION_STRING; }

int runApplication(int argc, char** argv) {
    App app;
    return app.run(argc, argv);
}

namespace {

constexpr float kBaseFontSize = 15.0f;
constexpr const char* kAppName = "SharkSDR";

void glfwErrorCallback(int code, const char* description) {
    logError("GLFW", "Error " + std::to_string(code) + ": " + description);
}

// Wayland does not let clients read or set the window position.
bool windowPositionSupported() {
#if defined(GLFW_PLATFORM_WAYLAND)
    return glfwGetPlatform() != GLFW_PLATFORM_WAYLAND;
#else
    return true;
#endif
}

// Fonts that contain CJK glyphs, merged into the UI font so that file names
// and plugin texts in Japanese/Chinese/Korean are displayed correctly.
std::vector<fs::path> cjkFontCandidates() {
#if defined(_WIN32)
    fs::path dir = "C:\\Windows\\Fonts";
    if (const char* w = std::getenv("WINDIR")) dir = fs::path(w) / "Fonts";
    return {dir / "YuGothM.ttc", dir / "meiryo.ttc", dir / "msgothic.ttc"};
#elif defined(__APPLE__)
    return {"/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
            "/Library/Fonts/Arial Unicode.ttf"};
#else
    return {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"};
#endif
}

const double kSnapSteps[] = {1,     10,     50,     100,    500,     1000,    2500,   5000,
                             6250,  8330,   9000,   10000,  12500,   25000,   50000,  100000, 200000};

// Draws a flat icon button and returns true when clicked.
bool iconButton(const char* id, const ImVec2& size, bool active, const std::function<void(ImDrawList*, ImVec2, ImVec2, ImU32)>& icon) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) dl->AddRectFilled(p, p + size, IM_COL32(64, 156, 255, 70), 5.0f);
    else if (hovered) dl->AddRectFilled(p, p + size, IM_COL32(255, 255, 255, 25), 5.0f);
    icon(dl, p, p + size, ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}

}  // namespace

// ---- construction -----------------------------------------------------------------

App::App() {
    audio_ = std::make_unique<AudioOutput>();
    engine_ = std::make_unique<Engine>(*audio_);
    spectrum_ = std::make_unique<SpectrumView>(*engine_);
    plugins_ = std::make_unique<PluginManager>(*this);
}

App::~App() { shutdown(); }

bool App::parseArguments(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--config") options_.configDir = utf8ToPath(next());
        else if (a == "--plugins") options_.pluginDirs.push_back(utf8ToPath(next()));
        else if (a == "--autostart") options_.autostart = true;
        else if (a == "--no-plugins") options_.noPlugins = true;
        else if (a == "--screenshot") options_.screenshot = utf8ToPath(next());
        else if (a == "--frames") options_.screenshotFrames = std::max(1, std::atoi(next().c_str()));
        else if (a == "--test-plugin-reload") options_.pluginReloadTest = true;
        else if (a == "--test-ui") options_.uiTest = true;
        else if (a == "--help" || a == "-h") {
            std::printf(
                "SharkSDR %s\n\n"
                "  --config DIR       use DIR for settings (default: per-user config folder)\n"
                "  --plugins DIR      also load plugins from DIR\n"
                "  --no-plugins       do not load any plugin\n"
                "  --autostart        start receiving immediately\n"
                "  --screenshot FILE  save a PNG of the window after --frames N frames and exit\n"
                "  --test-plugin-reload  (developer) unload and reload every plugin while running\n"
                "  --test-ui          (developer) inject mouse input and check the spectrum interactions\n",
                versionString());
            return false;
        }
    }
    return true;
}

// ---- configuration ----------------------------------------------------------------

void App::loadConfig() {
    configPath_ = configDirectory() / "config.json";
    std::ifstream in(configPath_);
    if (in) {
        try {
            in >> config_;
        } catch (const std::exception& e) {
            logError("Config", std::string("Cannot parse config.json: ") + e.what());
            config_ = nlohmann::json::object();
        }
    }
    if (!config_.is_object()) config_ = nlohmann::json::object();
    if (!config_.contains("radio")) {
        // First launch: tune the demo signal generator to its FM station.
        config_["radio"] = {{"frequency", 99.7e6}, {"center", 100.0e6}, {"mode", "WFM"}, {"source", "Signal Generator"}};
    }
    if (!config_.contains("pluginSettings") || !config_["pluginSettings"].is_object())
        config_["pluginSettings"] = nlohmann::json::object();

    const auto& ui = config_.value("ui", nlohmann::json::object());
    sidebarWidth_ = ui.value("sidebarWidth", sidebarWidth_);
    showSidebar_ = ui.value("showSidebar", showSidebar_);
    showStatusBar_ = ui.value("showStatusBar", showStatusBar_);
    uiScale_ = std::clamp(ui.value("scale", uiScale_), 0.5f, 3.0f);
    theme_ = ui.value("theme", std::string("dark")) == "light" ? ui::Theme::Light : ui::Theme::Dark;
    startOnLaunch_ = ui.value("startOnLaunch", startOnLaunch_);
    showPlugins_ = ui.value("showPlugins", showPlugins_);
    showLog_ = ui.value("showLog", showLog_);
    showShortcuts_ = ui.value("showShortcuts", showShortcuts_);
    if (ui.contains("panels") && ui["panels"].is_object()) panelOpen_ = ui["panels"];
}

void App::storeState() {
    nlohmann::json radio = nlohmann::json::object();
    engine_->saveState(radio);
    config_["radio"] = radio;
    nlohmann::json display = nlohmann::json::object();
    spectrum_->saveState(display);
    config_["display"] = display;

    nlohmann::json ui = config_.value("ui", nlohmann::json::object());
    ui["sidebarWidth"] = sidebarWidth_;
    ui["showSidebar"] = showSidebar_;
    ui["showStatusBar"] = showStatusBar_;
    ui["scale"] = uiScale_;
    ui["theme"] = theme_ == ui::Theme::Light ? "light" : "dark";
    ui["startOnLaunch"] = startOnLaunch_;
    ui["showPlugins"] = showPlugins_;
    ui["showLog"] = showLog_;
    ui["showShortcuts"] = showShortcuts_;
    ui["panels"] = panelOpen_;
    if (window_) {
        const bool maximized = glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) != 0;
        ui["maximized"] = maximized;
        if (!maximized) {
            int w, h;
            glfwGetWindowSize(window_, &w, &h);
            if (w > 100 && h > 100) {
                nlohmann::json win = {{"width", w}, {"height", h}};
                if (windowPositionSupported()) {
                    int x, y;
                    glfwGetWindowPos(window_, &x, &y);
                    win["x"] = x;
                    win["y"] = y;
                }
                ui["window"] = win;
            }
        }
    }
    config_["ui"] = ui;

    config_["audio"] = {{"device", audio_->currentDevice()}};
    nlohmann::json& enabled = config_["pluginsEnabled"];
    plugins_->storeEnabled(enabled);
}

void App::saveConfig() {
    storeState();
    const fs::path tmp = configPath_.string() + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            logError("Config", "Cannot write " + pathToUtf8(tmp));
            return;
        }
        out << config_.dump(2);
    }
    std::error_code ec;
    fs::rename(tmp, configPath_, ec);
    if (ec) {
        fs::remove(configPath_, ec);
        fs::rename(tmp, configPath_, ec);
    }
    lastSave_ = std::chrono::steady_clock::now();
}

nlohmann::json& App::pluginSettings(const std::string& pluginName) {
    nlohmann::json& all = config_["pluginSettings"];
    if (!all.is_object()) all = nlohmann::json::object();
    nlohmann::json& s = all[pluginName];
    if (!s.is_object()) s = nlohmann::json::object();
    return s;
}

// ---- registries -------------------------------------------------------------------

void App::addPanel(OwnerId owner, const std::string& title, std::function<void()> draw) {
    panels_.push_back({owner, title, std::move(draw)});
}

void App::addFrameCallback(OwnerId owner, std::function<void()> fn) { frameCallbacks_.push_back({owner, std::move(fn)}); }

void App::addMenu(OwnerId owner, std::function<void()> fn) { menus_.push_back({owner, std::move(fn)}); }

void App::addOverlay(OwnerId owner, OverlayFn fn) { overlays_.push_back({owner, std::move(fn)}); }

void App::removeOwner(OwnerId owner) {
    auto byOwner = [owner](const auto& e) { return e.owner == owner; };
    panels_.erase(std::remove_if(panels_.begin(), panels_.end(), byOwner), panels_.end());
    frameCallbacks_.erase(std::remove_if(frameCallbacks_.begin(), frameCallbacks_.end(), byOwner), frameCallbacks_.end());
    menus_.erase(std::remove_if(menus_.begin(), menus_.end(), byOwner), menus_.end());
    overlays_.erase(std::remove_if(overlays_.begin(), overlays_.end(), byOwner), overlays_.end());
    engine_->removeTaps(owner);
    engine_->removeSources(owner);
    engine_->unregisterDemodulators(owner);
}

// ---- window / ImGui setup -----------------------------------------------------------

bool App::initWindow() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        logError("GLFW", "glfwInit failed");
        return false;
    }
#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    (void)glsl;

    dpiScale_ = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    if (dpiScale_ <= 0.0f) dpiScale_ = 1.0f;

    const auto& ui = config_.value("ui", nlohmann::json::object());
    const auto& win = ui.value("window", nlohmann::json::object());
    const int w = win.value("width", int(1360 * dpiScale_));
    const int h = win.value("height", int(820 * dpiScale_));
    if (!options_.screenshot.empty()) glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    if (ui.value("maximized", false) && options_.screenshot.empty()) glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

    window_ = glfwCreateWindow(w, h, kAppName, nullptr, nullptr);
    if (!window_) {
        logError("GLFW", "Cannot create the window (OpenGL 3 required)");
        return false;
    }
    if (win.contains("x") && win.contains("y") && windowPositionSupported()) {
        // Only restore the position when it is visible on a monitor.
        int x = win.value("x", 100), y = win.value("y", 100);
        int count = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        for (int i = 0; i < count; i++) {
            int mx, my, mw, mh;
            glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
            if (x >= mx - 50 && y >= my - 10 && x < mx + mw - 100 && y < my + mh - 100) {
                glfwSetWindowPos(window_, x, y);
                break;
            }
        }
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);
    return true;
}

void App::loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const fs::path fontDir = executableDirectory() / "resources" / "fonts";
    ui::Fonts fonts;

    std::error_code ec;
    const fs::path roboto = fontDir / "Roboto-Medium.ttf";
    if (fs::exists(roboto, ec)) fonts.regular = io.Fonts->AddFontFromFileTTF(pathToUtf8(roboto).c_str(), kBaseFontSize);
    if (!fonts.regular) {
        logWarning("UI", "Roboto font not found, using the built-in font");
        fonts.regular = io.Fonts->AddFontDefault();
    }
    // Merges the first available CJK font into the font added last.
    auto mergeCjk = [&] {
        for (const auto& cjk : cjkFontCandidates()) {
            if (!fs::exists(cjk, ec)) continue;
            ImFontConfig cfg;
            cfg.MergeMode = true;
            if (io.Fonts->AddFontFromFileTTF(pathToUtf8(cjk).c_str(), kBaseFontSize, &cfg)) return;
        }
    };
    mergeCjk();
    const fs::path cousine = fontDir / "Cousine-Regular.ttf";
    if (fs::exists(cousine, ec)) {
        fonts.mono = io.Fonts->AddFontFromFileTTF(pathToUtf8(cousine).c_str(), kBaseFontSize);
        if (fonts.mono) mergeCjk();
    }
    if (!fonts.mono) fonts.mono = fonts.regular;
    ui::setFonts(fonts);
}

void App::setTheme(ui::Theme theme) {
    theme_ = theme;
    ui::applyTheme(theme_, dpiScale_ * uiScale_);
}

void App::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    iniPath_ = pathToUtf8(configDirectory() / "imgui.ini");
    io.IniFilename = iniPath_.c_str();
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    setTheme(theme_);
    loadFonts();

    // In the UI self test all input is injected: ignore the real mouse/keyboard.
    ImGui_ImplGlfw_InitForOpenGL(window_, !options_.uiTest);
#if defined(__APPLE__)
    ImGui_ImplOpenGL3_Init("#version 150");
#else
    ImGui_ImplOpenGL3_Init("#version 130");
#endif
}

void App::shutdown() {
    if (plugins_) plugins_->shutdownAll();
    if (engine_) engine_->stop();
    if (audio_) audio_->close();
    if (window_) {
        // GPU resources (the waterfall texture) must go while the context exists.
        spectrum_.reset();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
        window_ = nullptr;
    }
}

void App::captureScreenshot(const fs::path& path) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    std::vector<uint8_t> pixels(size_t(w) * size_t(h) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    std::vector<uint8_t> flipped(pixels.size());
    const size_t row = size_t(w) * 4;
    for (int y = 0; y < h; y++)
        std::copy_n(&pixels[size_t(h - 1 - y) * row], row, &flipped[size_t(y) * row]);
    for (size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
    if (writePng(path, w, h, flipped.data())) logInfo("App", "Screenshot saved to " + pathToUtf8(path));
    else logError("App", "Cannot write " + pathToUtf8(path));
}

// ---- main loop ----------------------------------------------------------------------

int App::run(int argc, char** argv) {
    if (!parseArguments(argc, argv)) return 0;
    if (!options_.configDir.empty()) setConfigDirectoryOverride(options_.configDir);
    if (options_.uiTest || options_.pluginReloadTest) setUserDataDirectoryOverride(configDirectory() / "data");
    logInfo("App", std::string("SharkSDR ") + versionString() + ", config: " + pathToUtf8(configDirectory()));

    loadConfig();
    if (!initWindow()) return 1;
    initImGui();

    audio_->init();
    audio_->open(config_.value("audio", nlohmann::json::object()).value("device", std::string()));

    engine_->loadState(config_["radio"]);
    spectrum_->loadState(config_.value("display", nlohmann::json::object()));

    if (!options_.noPlugins) {
        std::vector<fs::path> dirs = {executableDirectory() / "plugins", configDirectory() / "plugins"};
        for (const auto& d : options_.pluginDirs) dirs.push_back(d);
        plugins_->scan(dirs);
        plugins_->applyEnabled(config_.value("pluginsEnabled", nlohmann::json::object()));
    }
    if (!engine_->currentSource()) {
        auto names = engine_->sourceNames();
        if (!names.empty()) engine_->selectSource(names.front());
    }
    if (options_.autostart || startOnLaunch_) engine_->start();

    lastSave_ = std::chrono::steady_clock::now();
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        plugins_->processPending();
        engine_->update();
        spectrum_->update();

        if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED) != 0) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        if (options_.uiTest) uiTestStep();
        ImGui::NewFrame();

        handleShortcuts();
        drawFrame();
        updateWindowTitle();
        for (size_t i = 0; i < frameCallbacks_.size(); i++) frameCallbacks_[i].fn();
        if (showPlugins_) drawPluginsWindow();
        if (showLog_) drawLogWindow();
        if (showAbout_) drawAboutWindow();
        if (showShortcuts_) drawShortcutsWindow();
        if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);

        ImGui::Render();
        int fw, fh;
        glfwGetFramebufferSize(window_, &fw, &fh);
        glViewport(0, 0, fw, fh);
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        frameCount_++;
        if (options_.pluginReloadTest) {
            const int third = std::max(2, options_.screenshotFrames / 3);
            if (frameCount_ == third || frameCount_ == 2 * third) {
                const bool enable = frameCount_ == 2 * third;
                logInfo("App", enable ? "Reload test: enabling all plugins" : "Reload test: disabling all plugins");
                for (const auto& p : plugins_->plugins())
                    if (p->valid) plugins_->requestEnabled(*p, enable);
            }
            if (frameCount_ == 2 * third + 2) engine_->start();
        }
        if (!options_.screenshot.empty() && frameCount_ >= options_.screenshotFrames) {
            captureScreenshot(options_.screenshot);
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
        }
        glfwSwapBuffers(window_);

        if (std::chrono::steady_clock::now() - lastSave_ > std::chrono::seconds(60)) saveConfig();
    }

    saveConfig();
    shutdown();
    return uiTestFailures_ > 0 ? 2 : 0;
}

// What the stream hooks registered by the UI test saw (written on the DSP thread).
struct App::HookProbe {
    static constexpr OwnerId kOwner = 1 << 24;  // stands in for a plugin
    static constexpr float kTone = 0.25f;       // amplitude written into MonitorAF
    std::atomic<uint64_t> calls[6]{};           // per ProcessorType
    std::atomic<double> rate[6]{};
    std::atomic<double> channelFrequency{0.0};
    std::atomic<double> demodEnergy{0.0};
    std::atomic<float> outPeak{0.0f};  // after volume
    double tonePhase = 0.0;            // DSP thread
    // UI thread: snapshots taken when the bypass is set and the hooks are removed.
    double energyAtBypass = 0.0;
    uint64_t mpxAtBypass = 0, monitorAtBypass = 0, totalAtRemoval = 0;
    uint64_t total() const {
        uint64_t n = 0;
        for (const auto& c : calls) n += c.load();
        return n;
    }
};

// Developer self test: injects mouse/keyboard events into ImGui (no real
// input needed) and checks that the spectrum interactions change the state.
void App::uiTestStep() {
    ImGuiIO& io = ImGui::GetIO();
    // With macOS behaviours ImGui swaps Cmd and Ctrl in AddKeyEvent(), so send
    // the key that ends up as io.KeyCtrl.
    const ImGuiKey ctrlMod = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    const ImGuiKey ctrlKey = io.ConfigMacOSXBehaviors ? ImGuiKey_LeftSuper : ImGuiKey_LeftCtrl;
    const ImVec2 a = spectrum_->plotMin(), b = spectrum_->plotMax();
    const float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f;
    const double snap = engine_->snap();
    auto report = [&](const char* what, bool ok, const std::string& detail) {
        log(ok ? LogLevel::Info : LogLevel::Error, "UI test", std::string(ok ? "PASS " : "FAIL ") + what + " (" + detail + ")");
        if (!ok) uiTestFailures_++;
    };
    switch (frameCount_) {
        case 58:
            uiTestStartFreq_ = engine_->frequency();
            uiTestStartBw_ = engine_->bandwidth();
            break;
        case 60:  // click on the spectrum tunes the VFO there
            uiTestX_ = a.x + (b.x - a.x) * 0.3f;
            io.AddMousePosEvent(uiTestX_, cy);
            break;
        case 62: io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); break;
        case 64: io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); break;
        case 66: {
            const double expected = std::round(spectrum_->screenXToFrequency(uiTestX_) / snap) * snap;
            report("click to tune", std::fabs(engine_->frequency() - expected) <= snap,
                   ui::formatFrequency(engine_->frequency()) + ", expected " + ui::formatFrequency(expected));
            uiTestValue_ = engine_->frequency();
            io.AddMouseWheelEvent(0.0f, 1.0f);
            break;
        }
        case 68:
            report("mouse wheel tunes one step up", std::fabs(engine_->frequency() - (uiTestValue_ + snap)) < 1.0,
                   ui::formatFrequency(engine_->frequency()));
            break;
        case 70:
            uiTestValue_ = spectrum_->zoom();
            io.AddKeyEvent(ctrlMod, true);
            io.AddKeyEvent(ctrlKey, true);
            break;
        case 72: io.AddMouseWheelEvent(0.0f, 1.0f); break;
        case 74:
            io.AddKeyEvent(ctrlKey, false);
            io.AddKeyEvent(ctrlMod, false);
            break;
        case 76:
            report("ctrl + wheel zooms in", spectrum_->zoom() > uiTestValue_ * 1.1,
                   "zoom " + std::to_string(uiTestValue_) + " -> " + std::to_string(spectrum_->zoom()));
            break;
        case 80:  // drag the upper band edge to the right
            uiTestValue_ = engine_->bandwidth();
            uiTestX_ = spectrum_->frequencyToScreenX(engine_->frequency() + uiTestValue_ * 0.5);
            io.AddMousePosEvent(uiTestX_, cy);
            break;
        case 82: io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); break;
        case 84: io.AddMousePosEvent(uiTestX_ + 40.0f, cy); break;
        case 86: io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); break;
        case 88:
            report("dragging the band edge widens the bandwidth", engine_->bandwidth() > uiTestValue_ * 1.05,
                   ui::formatBandwidth(uiTestValue_) + " -> " + ui::formatBandwidth(engine_->bandwidth()));
            break;
        case 90:  // right drag pans the zoomed view
            uiTestValue_ = spectrum_->viewOffset();
            io.AddMousePosEvent(cx, cy);
            break;
        case 92: io.AddMouseButtonEvent(ImGuiMouseButton_Right, true); break;
        case 94: io.AddMousePosEvent(cx - 100.0f, cy); break;
        case 96: io.AddMouseButtonEvent(ImGuiMouseButton_Right, false); break;
        case 98:
            report("right drag pans the view", std::fabs(spectrum_->viewOffset() - uiTestValue_) > 1.0,
                   "offset " + std::to_string(uiTestValue_) + " -> " + std::to_string(spectrum_->viewOffset()));
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        case 99:  // back to the starting point before recording
            engine_->setFrequency(uiTestStartFreq_);
            engine_->setBandwidth(uiTestStartBw_);
            break;
        // Typed slider values: double-click the volume slider, type, Enter.
        case 110:
        case 160:
            if (frameCount_ == 110) uiTestValue_ = engine_->volume();
            io.AddMousePosEvent(uiTestVolumeMin_.x + (uiTestVolumeMax_.x - uiTestVolumeMin_.x) * 0.25f,
                                (uiTestVolumeMin_.y + uiTestVolumeMax_.y) * 0.5f);
            break;
        case 112:
        case 114:
        case 128:
        case 130:
        case 162:
        case 164: io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); break;
        case 113:
        case 115:
        case 129:
        case 131:
        case 163:
        case 165: io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); break;
        case 118:
            report("double-click on a slider keeps its value", std::fabs(engine_->volume() - uiTestValue_) < 1e-4,
                   std::to_string(uiTestValue_) + " -> " + std::to_string(engine_->volume()));
            io.AddInputCharactersUTF8("35");
            break;
        case 120:
        case 136: io.AddKeyEvent(ImGuiKey_Enter, true); break;
        case 121:
        case 137: io.AddKeyEvent(ImGuiKey_Enter, false); break;
        case 124:
            report("typing a value into a slider", std::fabs(engine_->volume() - 0.35f) < 1e-4,
                   "volume " + std::to_string(engine_->volume()));
            break;
        case 126:  // vertical slider (spectrum max level): the field opens in a popup
            uiTestValue2_ = spectrum_->maxDb();
            io.AddMousePosEvent(spectrum_->maxSliderCenter().x, spectrum_->maxSliderCenter().y);
            break;
        case 134: io.AddInputCharactersUTF8("-30"); break;
        case 140:
            report("typing a value into a vertical slider", std::fabs(spectrum_->maxDb() + 30.0f) < 0.01f,
                   "max " + std::to_string(uiTestValue2_) + " -> " + std::to_string(spectrum_->maxDb()) + " dB");
            break;
        // Esc cancels: double-click the volume slider again (160-165), type, Esc.
        case 168: io.AddInputCharactersUTF8("90"); break;
        case 170: io.AddKeyEvent(ImGuiKey_Escape, true); break;
        case 171: io.AddKeyEvent(ImGuiKey_Escape, false); break;
        case 174:
            report("Esc cancels the typed value", std::fabs(engine_->volume() - 0.35f) < 1e-4,
                   "volume " + std::to_string(engine_->volume()));
            engine_->setVolume(float(uiTestValue_));
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        // Stream hooks at every point, registered the way a decoder plugin would.
        case 176: {
            if (engine_->mode() != "WFM") engine_->setMode("WFM");
            hookProbe_ = std::make_shared<HookProbe>();
            auto probe = hookProbe_;
            const OwnerId owner = HookProbe::kOwner;
            auto note = [probe](const StreamInfo& info) {
                probe->calls[int(info.type)]++;
                probe->rate[int(info.type)] = info.sampleRate;
            };
            for (ProcessorType t : {ProcessorType::RawIQ, ProcessorType::DecimatedAndFilteredIQ})
                engine_->addStreamHook(t, IqProcessor([probe, note](Complex*, size_t, const StreamInfo& info) {
                    note(info);
                    if (info.type == ProcessorType::DecimatedAndFilteredIQ) probe->channelFrequency = info.frequency;
                }), owner);
            engine_->addStreamHook(ProcessorType::FMMPX, RealProcessor([note](float*, size_t, const StreamInfo& info) { note(info); }),
                                   owner);
            engine_->addStreamHook(ProcessorType::DemodulatorOutput,
                                   AudioProcessor([probe, note](Stereo* f, size_t n, const StreamInfo& info) {
                                       note(info);
                                       double e = 0.0;
                                       for (size_t i = 0; i < n; i++) e += double(f[i].l) * f[i].l;
                                       probe->demodEnergy = probe->demodEnergy.load() + e;
                                   }),
                                   owner);
            engine_->addStreamHook(ProcessorType::FilteredAudioOutput,
                                   AudioProcessor([note](Stereo*, size_t, const StreamInfo& info) { note(info); }), owner);
            // Replace what is played with a tone, as a decoder plays its voice.
            engine_->addStreamHook(ProcessorType::MonitorAF, AudioProcessor([probe, note](Stereo* f, size_t n, const StreamInfo& info) {
                note(info);
                for (size_t i = 0; i < n; i++) {
                    f[i] = Stereo(HookProbe::kTone * float(std::sin(probe->tonePhase)));
                    probe->tonePhase = std::fmod(probe->tonePhase + kTwoPi * 1000.0 / info.sampleRate, kTwoPi);
                }
            }), owner);
            engine_->addAudioTap([probe](const Stereo* f, size_t n, double) {
                float peak = probe->outPeak.load();
                for (size_t i = 0; i < n; i++) peak = std::max(peak, std::fabs(f[i].l));
                probe->outPeak = peak;
            }, owner);
            const TapHandle wrong =
                engine_->addStreamHook(ProcessorType::MonitorAF, IqProcessor([](Complex*, size_t, const StreamInfo&) {}), owner);
            report("a hook with the wrong sample type is refused", wrong == 0, "handle " + std::to_string(wrong));
            break;
        }
        case 196: {
            HookProbe& p = *hookProbe_;
            static const char* const names[] = {"RawIQ", "DecimatedAndFilteredIQ", "FMMPX", "DemodulatorOutput",
                                                "FilteredAudioOutput", "MonitorAF"};
            const double audioRate = audio_->sampleRate();
            const double expected[] = {engine_->sampleRate(), 250000.0, 250000.0, 250000.0, audioRate, audioRate};
            bool ok = true;
            std::string detail;
            for (int i = 0; i < 6; i++) {
                ok = ok && p.calls[i] > 0 && p.rate[i] == expected[i];
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s%s %llux %.0f Hz", i ? ", " : "", names[i],
                              (unsigned long long)p.calls[i].load(), p.rate[i].load());
                detail += buf;
            }
            report("stream hooks run at every point, at their sample rates", ok, detail);
            report("DecimatedAndFilteredIQ is centred on the VFO", std::fabs(p.channelFrequency - engine_->frequency()) < 1.0,
                   ui::formatFrequency(p.channelFrequency) + " / VFO " + ui::formatFrequency(engine_->frequency()));
            report("DemodulatorOutput carries the demodulated audio", p.demodEnergy > 0.0,
                   "energy " + std::to_string(p.demodEnergy.load()));
            const float gain = engine_->muted() ? 0.0f : engine_->volume() * engine_->volume();
            const float want = HookProbe::kTone * gain;
            report("audio written in MonitorAF is played, with the volume applied after it",
                   std::fabs(p.outPeak - want) <= 0.01f * HookProbe::kTone,
                   "peak " + std::to_string(p.outPeak.load()) + ", expected " + std::to_string(want));
            engine_->setBypassDemodulation(HookProbe::kOwner, true);
            p.energyAtBypass = p.demodEnergy;
            p.mpxAtBypass = p.calls[int(ProcessorType::FMMPX)];
            p.monitorAtBypass = p.calls[int(ProcessorType::MonitorAF)];
            break;
        }
        case 206: {
            HookProbe& p = *hookProbe_;
            const uint64_t mpx = p.calls[int(ProcessorType::FMMPX)], monitor = p.calls[int(ProcessorType::MonitorAF)];
            report("bypassing the demodulator silences it, MonitorAF still runs",
                   engine_->bypassDemodulation() && p.demodEnergy == p.energyAtBypass && mpx == p.mpxAtBypass &&
                       monitor > p.monitorAtBypass,
                   "FMMPX " + std::to_string(mpx - p.mpxAtBypass) + " calls, MonitorAF " +
                       std::to_string(monitor - p.monitorAtBypass) + " calls since");
            removeOwner(HookProbe::kOwner);  // what unloading the plugin does
            p.totalAtRemoval = p.total();
            break;
        }
        case 214:
            report("removed hooks are not called again and the bypass is released",
                   hookProbe_->total() == hookProbe_->totalAtRemoval && !engine_->bypassDemodulation(),
                   std::to_string(hookProbe_->total() - hookProbe_->totalAtRemoval) + " calls after removal");
            hookProbe_.reset();
            break;
        // Ctrl+R twice: handled by the Recorder plugin (start, then stop ~1 s later).
        case 100:
        case 200:
            io.AddKeyEvent(ctrlMod, true);
            io.AddKeyEvent(ctrlKey, true);
            break;
        case 101:
        case 201: io.AddKeyEvent(ImGuiKey_R, true); break;
        case 102:
        case 202:
            io.AddKeyEvent(ImGuiKey_R, false);
            io.AddKeyEvent(ctrlKey, false);
            io.AddKeyEvent(ctrlMod, false);
            break;
        default: break;
    }
}

void App::updateWindowTitle() {
    std::string title = std::string(kAppName) + " - " + ui::formatFrequency(engine_->frequency()) + " " + engine_->mode();
    if (title != windowTitle_) {
        windowTitle_ = title;
        glfwSetWindowTitle(window_, title.c_str());
    }
}

// ---- shortcuts ----------------------------------------------------------------------

void App::tuneStep(int direction) {
    const double snap = engine_->snap();
    const double f = std::round(engine_->frequency() / snap) * snap + direction * snap;
    engine_->setFrequency(f);
}

void App::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) engine_->running() ? engine_->stop() : (void)engine_->start();
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) tuneStep(+1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) tuneStep(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_M, false) && !io.KeyCtrl) engine_->setMuted(!engine_->muted());
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) saveConfig();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

// ---- layout -------------------------------------------------------------------------

void App::drawFrame() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus);
    ImGui::PopStyleVar(3);

    drawMenuBar();

    const float toolbarH = std::floor(ImGui::GetFontSize() * 2.0f + 18.0f * dpiScale_ * uiScale_);
    drawToolbar(toolbarH);

    const float statusH = showStatusBar_ ? ImGui::GetFrameHeight() : 0.0f;
    const float contentH = std::max(50.0f, ImGui::GetContentRegionAvail().y - statusH);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (showSidebar_) {
        sidebarWidth_ = std::clamp(sidebarWidth_, 180.0f, std::max(200.0f, ImGui::GetContentRegionAvail().x * 0.6f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme_ == ui::Theme::Dark ? ImVec4(0.09f, 0.098f, 0.112f, 1.0f)
                                                                          : ImVec4(0.92f, 0.93f, 0.945f, 1.0f));
        ImGui::BeginChild("##sidebar", ImVec2(sidebarWidth_, contentH), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 5));
        drawSidebar();
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::SameLine();
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##sidesplit", ImVec2(5.0f, contentH));
        const bool splitActive = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() || splitActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (splitActive) sidebarWidth_ += ImGui::GetIO().MouseDelta.x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(sp.x + 2.0f, sp.y), ImVec2(sp.x + 2.0f, sp.y + contentH),
                                            splitActive || ImGui::IsItemHovered() ? ui::colors::kAccent
                                                                                  : ImGui::GetColorU32(ImGuiCol_Border));
        ImGui::SameLine();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::BeginChild("##mainarea", ImVec2(0, contentH), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
    std::vector<const OverlayFn*> overlays;
    for (const auto& o : overlays_) overlays.push_back(&o.fn);
    spectrum_->draw(ImGui::GetContentRegionAvail(), overlays);
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (showStatusBar_) drawStatusBar(statusH);
    ImGui::End();
}

void App::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem(engine_->running() ? "Stop" : "Start", "Space")) {
            if (engine_->running()) engine_->stop();
            else engine_->start();
        }
        ImGui::MenuItem("Start on launch", nullptr, &startOnLaunch_);
        ImGui::Separator();
        if (ImGui::MenuItem("Save settings", SHARKSDR_CTRL_KEY "+S")) saveConfig();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", SHARKSDR_CTRL_KEY "+Q")) glfwSetWindowShouldClose(window_, GLFW_TRUE);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Sidebar", nullptr, &showSidebar_);
        ImGui::MenuItem("Status bar", nullptr, &showStatusBar_);
        if (ImGui::BeginMenu("Theme")) {
            if (ImGui::MenuItem("Dark", nullptr, theme_ == ui::Theme::Dark)) setTheme(ui::Theme::Dark);
            if (ImGui::MenuItem("Light", nullptr, theme_ == ui::Theme::Light)) setTheme(ui::Theme::Light);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("UI scale")) {
            for (float s : {0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f}) {
                char label[16];
                std::snprintf(label, sizeof(label), "%.0f %%", s * 100.0f);
                if (ImGui::MenuItem(label, nullptr, std::fabs(uiScale_ - s) < 0.01f)) {
                    uiScale_ = s;
                    setTheme(theme_);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Log", nullptr, &showLog_);
        ImGui::MenuItem("ImGui demo", nullptr, &showDemo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        if (menus_.empty()) ImGui::TextDisabled("No tools (plugins can add entries here)");
        for (size_t i = 0; i < menus_.size(); i++) {
            ImGui::PushID(int(i));
            menus_[i].fn();
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Plugins")) {
        ImGui::MenuItem("Plugin manager...", nullptr, &showPlugins_);
        ImGui::Separator();
        for (const auto& p : plugins_->plugins()) {
            if (!p->valid) continue;
            ImGui::MenuItem(p->name.c_str(), p->version.c_str(), p->active, false);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Keyboard & mouse", nullptr, &showShortcuts_);
        ImGui::MenuItem("About SharkSDR", nullptr, &showAbout_);
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void App::drawToolbar(float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme_ == ui::Theme::Dark ? ImVec4(0.063f, 0.07f, 0.08f, 1.0f)
                                                                      : ImVec4(0.88f, 0.89f, 0.91f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 0));
    ImGui::BeginChild("##toolbar", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const float btn = std::floor(height * 0.62f);
    const float btnY = (height - btn) * 0.5f;
    const ImVec2 bsize(btn, btn);

    // Sidebar toggle
    ImGui::SetCursorPosY(btnY);
    if (iconButton("##menu", bsize, false, [](ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
            const float w = b.x - a.x, h = b.y - a.y;
            for (int i = -1; i <= 1; i++) {
                const float y = a.y + h * 0.5f + float(i) * h * 0.2f;
                dl->AddLine(ImVec2(a.x + w * 0.25f, y), ImVec2(b.x - w * 0.25f, y), col, 2.0f);
            }
        }))
        showSidebar_ = !showSidebar_;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show/hide the sidebar");

    // Play / stop
    ImGui::SameLine();
    ImGui::SetCursorPosY(btnY);
    const bool running = engine_->running();
    if (iconButton("##play", bsize, running, [running](ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
            const ImVec2 c = (a + b) * 0.5f;
            const float s = (b.x - a.x) * 0.22f;
            if (running) dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, 2.0f);
            else dl->AddTriangleFilled(ImVec2(c.x - s * 0.8f, c.y - s * 1.1f), ImVec2(c.x - s * 0.8f, c.y + s * 1.1f),
                                       ImVec2(c.x + s * 1.2f, c.y), IM_COL32(90, 210, 120, 255));
        })) {
        if (running) engine_->stop();
        else engine_->start();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(running ? "Stop (Space)" : "Start (Space)");

    // Mute + volume
    ImGui::SameLine(0, 14.0f);
    ImGui::SetCursorPosY(btnY);
    const bool muted = engine_->muted();
    if (iconButton("##mute", bsize, false, [muted](ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
            const float w = b.x - a.x, h = b.y - a.y;
            const float x0 = a.x + w * 0.22f, y0 = a.y + h * 0.5f;
            dl->AddRectFilled(ImVec2(x0, y0 - h * 0.1f), ImVec2(x0 + w * 0.14f, y0 + h * 0.1f), col);
            dl->AddTriangleFilled(ImVec2(x0 + w * 0.1f, y0), ImVec2(x0 + w * 0.36f, y0 - h * 0.26f),
                                  ImVec2(x0 + w * 0.36f, y0 + h * 0.26f), col);
            if (muted) {
                dl->AddLine(ImVec2(a.x + w * 0.64f, y0 - h * 0.12f), ImVec2(a.x + w * 0.84f, y0 + h * 0.12f), IM_COL32(240, 90, 80, 255), 2.0f);
                dl->AddLine(ImVec2(a.x + w * 0.64f, y0 + h * 0.12f), ImVec2(a.x + w * 0.84f, y0 - h * 0.12f), IM_COL32(240, 90, 80, 255), 2.0f);
            } else {
                dl->PathArcTo(ImVec2(x0 + w * 0.3f, y0), w * 0.28f, -0.8f, 0.8f, 8);
                dl->PathStroke(col, 1.5f);
                dl->PathArcTo(ImVec2(x0 + w * 0.3f, y0), w * 0.42f, -0.8f, 0.8f, 8);
                dl->PathStroke(col, 1.5f);
            }
        }))
        engine_->setMuted(!muted);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(muted ? "Unmute (M)" : "Mute (M)");

    ImGui::SameLine();
    ImGui::SetCursorPosY((height - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    float vol = engine_->volume() * 100.0f;
    if (ui::SliderFloat("##volume", &vol, 0.0f, 100.0f, "%.0f %%")) engine_->setVolume(vol / 100.0f);
    uiTestVolumeMin_ = ImGui::GetItemRectMin();
    uiTestVolumeMax_ = ImGui::GetItemRectMax();

    // Frequency dial
    ImGui::SameLine(0, 24.0f);
    const float dialSize = ImGui::GetStyle().FontSizeBase * 2.0f;
    const float dialH = ImGui::GetFontSize() * 2.0f;
    ImGui::SetCursorPosY((height - dialH) * 0.5f);
    double freq = engine_->frequency();
    if (ui::FrequencyDial("##freq", &freq, 0.0, 999999999999.0, dialSize)) engine_->setFrequency(freq);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scroll or click the upper/lower half of a digit to tune.\nRight click zeroes the digits to the right.");

    // Mode / bandwidth summary
    ImGui::SameLine(0, 18.0f);
    {
        const float lineH = ImGui::GetTextLineHeight();
        const float y0 = (height - lineH * 2.0f - 2.0f) * 0.5f;
        const float x = ImGui::GetCursorPosX();
        ImGui::SetCursorPosY(y0);
        std::string status = engine_->demodulatorStatus();
        ImGui::TextColored(ImVec4(0.45f, 0.72f, 1.0f, 1.0f), "%s", engine_->mode().c_str());
        if (!status.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status.c_str());
        }
        ImGui::SetCursorPos(ImVec2(x, y0 + lineH + 2.0f));
        ImGui::TextDisabled("BW %s", ui::formatBandwidth(engine_->bandwidth()).c_str());
    }

    // Signal meter (right aligned)
    const float meterW = ImGui::GetFontSize() * 13.0f;
    const float right = ImGui::GetWindowWidth() - meterW - 16.0f;
    ImGui::SameLine();
    if (ImGui::GetCursorPosX() < right) ImGui::SetCursorPosX(right);
    {
        const float lineH = ImGui::GetTextLineHeight();
        const float meterH = std::floor(lineH * 0.6f);
        const float y0 = (height - lineH - meterH - 4.0f) * 0.5f;
        const float level = engine_->signalLevel();
        ImGui::SetCursorPosY(y0);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(p, ImGui::GetColorU32(ImGuiCol_TextDisabled), "Signal");
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f dBFS", level < -149.0f ? -150.0f : level);
        dl->AddText(ImVec2(p.x + meterW - ImGui::CalcTextSize(buf).x, p.y), ImGui::GetColorU32(ImGuiCol_Text), buf);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lineH + 4.0f));
        ui::LevelMeter("##smeter", level, -120.0f, 0.0f, ImVec2(meterW, meterH),
                       engine_->squelchEnabled() ? engine_->squelchLevel() : -1e30f);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::drawStatusBar(float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme_ == ui::Theme::Dark ? ImVec4(0.063f, 0.07f, 0.08f, 1.0f)
                                                                      : ImVec4(0.88f, 0.89f, 0.91f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 0));
    ImGui::BeginChild("##status", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::SetCursorPosY((height - ImGui::GetTextLineHeight()) * 0.5f);

    const bool running = engine_->running();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = ImGui::GetTextLineHeight() * 0.28f;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f), r,
                                                running ? IM_COL32(80, 210, 120, 255) : IM_COL32(120, 125, 135, 255));
    ImGui::Dummy(ImVec2(r * 2.0f + 2.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine();
    ImGui::TextUnformatted(running ? "Running" : "Stopped");

    auto sep = [] {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
    };
    sep();
    const std::string src = engine_->currentSourceName();
    ImGui::Text("%s", src.empty() ? "No source" : src.c_str());
    sep();
    ImGui::Text("%s", engine_->sampleRate() > 0 ? ui::formatSampleRate(engine_->sampleRate()).c_str() : "-");
    sep();
    ImGui::Text("Center %s", ui::formatFrequency(engine_->centerFrequency()).c_str());
    sep();
    ImGui::Text("DSP %.0f %%", engine_->dspLoad() * 100.0f);
    sep();
    ImGui::Text("Audio %.0f ms", audio_->bufferedMs());
    if (engine_->droppedSamples() > 0) {
        sep();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Dropped %llu", (unsigned long long)engine_->droppedSamples());
    }
    if (!engine_->lastError().empty()) {
        sep();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", engine_->lastError().c_str());
    }

    char fps[32];
    std::snprintf(fps, sizeof(fps), "%.0f FPS", ImGui::GetIO().Framerate);
    const float fw = ImGui::CalcTextSize(fps).x;
    ImGui::SameLine();
    const float x = ImGui::GetWindowWidth() - fw - 10.0f;
    if (ImGui::GetCursorPosX() < x) ImGui::SetCursorPosX(x);
    ImGui::TextDisabled("%s", fps);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---- sidebar ------------------------------------------------------------------------

bool App::beginPanel(const char* title, bool defaultOpen) {
    const bool open = panelOpen_.value(title, defaultOpen);
    ImGui::SetNextItemOpen(open, ImGuiCond_Once);
    const bool now = ImGui::CollapsingHeader(title);
    panelOpen_[title] = now;
    return now;
}

void App::drawSidebar() {
    if (beginPanel("Source")) drawSourcePanel();
    if (beginPanel("Radio")) drawRadioPanel();
    if (beginPanel("Audio", false)) drawAudioPanel();
    if (beginPanel("Display", false)) drawDisplayPanel();
    for (size_t i = 0; i < panels_.size(); i++) {
        ImGui::PushID(int(i) + 1000);
        if (beginPanel(panels_[i].title.c_str(), false)) {
            panels_[i].draw();
            ImGui::Spacing();
        }
        ImGui::PopID();
    }
}

void App::drawSourcePanel() {
    const auto names = engine_->sourceNames();
    const std::string current = engine_->currentSourceName();
    ui::LabeledRow("Source");
    if (ImGui::BeginCombo("##source", current.empty() ? "(none)" : current.c_str())) {
        for (const auto& n : names)
            if (ImGui::Selectable(n.c_str(), n == current)) engine_->selectSource(n);
        ImGui::EndCombo();
    }
    if (names.empty()) {
        ImGui::TextWrapped("No source available. Source plugins are loaded from:");
        ImGui::TextDisabled("%s", pathToUtf8(executableDirectory() / "plugins").c_str());
    }

    if (Source* s = engine_->currentSource()) {
        ImGui::PushID(current.c_str());
        s->drawSettings(engine_->running());
        ImGui::PopID();

        if (s->tunable()) {
            double center = engine_->centerFrequency();
            ui::LabeledRow("Center");
            if (ui::InputFrequency("##center", &center)) engine_->setCenterFrequency(center);
            bool ct = engine_->centerTuning();
            if (ImGui::Checkbox("Center tuning", &ct)) engine_->setCenterTuning(ct);
            ImGui::SameLine();
            ui::HelpMarker("Always retune the hardware so that the VFO stays in the middle of the spectrum.");
        }
    }
    bool dc = engine_->dcBlock();
    if (ImGui::Checkbox("Remove DC offset", &dc)) engine_->setDcBlock(dc);
    ui::SameLineColumn();
    bool iq = engine_->iqCorrection();
    if (ImGui::Checkbox("IQ correction", &iq)) engine_->setIqCorrection(iq);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Removes the mirror images of strong stations caused by I/Q gain and phase errors\n"
                          "(zero-IF tuners such as the E4000 and FC0012/FC0013)");
    bool swap = engine_->swapIq();
    if (ImGui::Checkbox("Swap I/Q", &swap)) engine_->setSwapIq(swap);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mirror the spectrum (for hardware with swapped I and Q)");

    if (!engine_->lastError().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", engine_->lastError().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

void App::drawRadioPanel() {
    double freq = engine_->frequency();
    ui::LabeledRow("Frequency");
    if (ui::InputFrequency("##freqinput", &freq)) engine_->setFrequency(freq);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Type e.g. 145.5M, 7100k or 1090000 and press Enter");

    // Mode buttons in a grid.
    const auto modes = engine_->modes();
    const std::string mode = engine_->mode();
    const int perRow = 4;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float bw = std::floor((ImGui::GetContentRegionAvail().x - spacing * (perRow - 1)) / perRow);
    for (size_t i = 0; i < modes.size(); i++) {
        if (i % perRow != 0) ImGui::SameLine();
        if (ui::ToggleButton(modes[i].c_str(), modes[i] == mode, ImVec2(bw, 0))) engine_->setMode(modes[i]);
    }
    ImGui::Spacing();

    float bandwidth = float(engine_->bandwidth());
    const float minBw = float(engine_->minBandwidth()), maxBw = float(engine_->maxBandwidth());
    ui::LabeledRow("Bandwidth");
    if (ui::SliderFloat("##bw", &bandwidth, minBw, maxBw, ui::formatBandwidth(bandwidth).c_str(),
                        ImGuiSliderFlags_Logarithmic))
        engine_->setBandwidth(std::round(bandwidth));

    const double snap = engine_->snap();
    ui::LabeledRow("Step");
    if (ImGui::BeginCombo("##snap", ui::formatBandwidth(snap).c_str())) {
        for (double s : kSnapSteps)
            if (ImGui::Selectable(ui::formatBandwidth(s).c_str(), s == snap)) engine_->setSnap(s);
        ImGui::EndCombo();
    }

    bool sq = engine_->squelchEnabled();
    float level = engine_->squelchLevel();
    if (ui::CheckboxRow("Squelch", &sq)) engine_->setSquelch(sq, level);
    ImGui::BeginDisabled(!sq);
    if (ui::SliderFloat("##sqlevel", &level, -120.0f, 0.0f, "%.0f dB")) engine_->setSquelch(sq, level);
    ImGui::EndDisabled();

    // Signal level, its label lined up with the checkbox labels around it.
    {
        const float x0 = ImGui::GetCursorPosX();
        const float textIndent = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetCursorPosX(x0 + textIndent);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Level");
        ImGui::SameLine(x0 + ui::LabelWidth());
        const float meterH = std::floor(ImGui::GetTextLineHeight() * 0.7f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((ImGui::GetFrameHeight() - meterH) * 0.5f));
        ui::LevelMeter("##level", engine_->signalLevel(), -120.0f, 0.0f, ImVec2(-1.0f, meterH), sq ? level : -1e30f);
    }

    // Audio post filters (per mode).
    AudioFilterSettings filters = engine_->audioFilters();
    bool filtersChanged = false;
    filtersChanged |= ui::CheckboxRow("High pass", &filters.highpass);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Removes rumble and sub-audible tones (CTCSS around 67-250 Hz: use ~300 Hz)");
    ImGui::BeginDisabled(!filters.highpass);
    filtersChanged |= ui::SliderFloat("##hpf", &filters.highpassHz, 20.0f, 1000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    filtersChanged |= ui::CheckboxRow("Low pass", &filters.lowpass);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cuts everything above the cutoff. FM hiss is strongest at high audio\n"
                          "frequencies, so 6-10 kHz makes weak broadcast stations much quieter.");
    ImGui::BeginDisabled(!filters.lowpass);
    filtersChanged |= ui::SliderFloat("##lpf", &filters.lowpassHz, 500.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    filtersChanged |= ui::CheckboxRow("Audio NR", &filters.noiseReduction);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Noise reduction: learns the background hiss (in about a second) and removes it\n"
                          "from the audio. The slider sets how far the noise is pushed down.");
    ImGui::BeginDisabled(!filters.noiseReduction);
    filtersChanged |= ui::SliderFloat("##nr", &filters.noiseReductionDb, 3.0f, 30.0f, "%.0f dB");
    ImGui::EndDisabled();
    if (filtersChanged) engine_->setAudioFilters(filters);

    ImGui::Separator();
    engine_->drawDemodulatorSettings();
    ImGui::Spacing();
}

void App::drawAudioPanel() {
    const auto& devices = audio_->deviceNames();
    const std::string& cur = audio_->currentDevice();
    ui::LabeledRow("Device");
    const float refreshW = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - refreshW - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##device", cur.empty() ? "Default" : cur.c_str())) {
        if (ImGui::Selectable("Default", cur.empty())) audio_->open("");
        for (const auto& d : devices)
            if (ImGui::Selectable(d.c_str(), d == cur)) audio_->open(d);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) audio_->refreshDevices();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look for new audio devices");

    float vol = engine_->volume() * 100.0f;
    ui::LabeledRow("Volume");
    if (ui::SliderFloat("##vol2", &vol, 0.0f, 100.0f, "%.0f %%")) engine_->setVolume(vol / 100.0f);
    bool muted = engine_->muted();
    if (ImGui::Checkbox("Mute", &muted)) engine_->setMuted(muted);
    ImGui::TextDisabled("Buffer %.0f ms, underruns %llu", audio_->bufferedMs(), (unsigned long long)audio_->underruns());
    ImGui::Spacing();
}

void App::drawDisplayPanel() {
    spectrum_->drawSettings();
    ImGui::Spacing();
}

// ---- windows ------------------------------------------------------------------------

void App::drawPluginsWindow() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 48.0f, ImGui::GetFontSize() * 24.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Plugin manager", &showPlugins_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Plugins extend SharkSDR with sources, demodulators and tools. "
                       "Enable or disable them here; the change takes effect immediately.");
    ImGui::Spacing();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##plugins", 5, flags, ImVec2(0, -ImGui::GetTextLineHeightWithSpacing() * 3.5f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        int i = 0;
        for (const auto& p : plugins_->plugins()) {
            ImGui::PushID(i++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool enabled = p->enabled;
            ImGui::BeginDisabled(!p->valid);
            if (ImGui::Checkbox("##en", &enabled)) plugins_->requestEnabled(*p, enabled);
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p->valid ? p->name.c_str() : p->fileName.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n\n%s", p->description.empty() ? "(no description)" : p->description.c_str(),
                                  pathToUtf8(p->path).c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p->version.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p->author.c_str());
            ImGui::TableNextColumn();
            if (!p->error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", p->error.c_str());
            else if (p->active) ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.5f, 1.0f), "Active");
            else ImGui::TextDisabled("Disabled");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Plugin folders (new files are detected at startup):");
    for (const auto& d : plugins_->directories()) ImGui::BulletText("%s", pathToUtf8(d).c_str());
    ImGui::End();
}

void App::drawLogWindow() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 20.0f, vp->WorkPos.y + vp->WorkSize.y - 40.0f),
                            ImGuiCond_FirstUseEver, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 50.0f, ImGui::GetFontSize() * 20.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log", &showLog_)) {
        ImGui::End();
        return;
    }
    for (auto& e : logEntries(logCursor_, &logCursor_)) logLines_.push_back(std::move(e));
    if (logLines_.size() > 2000) logLines_.erase(logLines_.begin(), logLines_.begin() + std::ptrdiff_t(logLines_.size() - 2000));
    if (ImGui::Button("Clear")) logLines_.clear();
    ImGui::Separator();
    ImGui::BeginChild("##logscroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(ui::fonts().mono, 0.0f);
    for (const auto& e : logLines_) {
        ImVec4 col = ImGui::GetStyle().Colors[ImGuiCol_Text];
        if (e.level == LogLevel::Warning) col = ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
        if (e.level == LogLevel::Error) col = ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
        if (e.level == LogLevel::Debug) col = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        ImGui::TextColored(col, "[%8.2f] %s: %s", e.time, e.source.c_str(), e.message.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::End();
}

void App::drawAboutWindow() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 28.0f, 0), ImGuiCond_Always);
    if (!ImGui::Begin("About SharkSDR", &showAbout_, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
    ImGui::TextColored(ImVec4(0.45f, 0.72f, 1.0f, 1.0f), "SharkSDR");
    ImGui::PopFont();
    ImGui::Text("Version %s", versionString());
    ImGui::TextWrapped("A cross-platform, plugin based software defined radio receiver.");
    ImGui::Separator();
    ImGui::TextDisabled("Built with");
    ImGui::BulletText("Dear ImGui %s (MIT)", IMGUI_VERSION);
    ImGui::BulletText("GLFW %d.%d.%d (zlib)", GLFW_VERSION_MAJOR, GLFW_VERSION_MINOR, GLFW_VERSION_REVISION);
    ImGui::BulletText("miniaudio (MIT-0 / public domain)");
    ImGui::BulletText("nlohmann/json (MIT)");
    ImGui::BulletText("Roboto & Cousine fonts (Apache 2.0)");
    ImGui::Separator();
    ImGui::TextDisabled("Config: %s", pathToUtf8(configDirectory()).c_str());
    ImGui::TextDisabled("Plugin ABI: %s", SHARKSDR_BUILD_SIGNATURE);
    ImGui::End();
}

void App::drawShortcutsWindow() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.35f, vp->WorkPos.y + vp->WorkSize.y * 0.3f),
                            ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Keyboard & mouse", &showShortcuts_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        const char* rows[][2] = {
            {"Space", "Start / stop"},
            {"Left / Right", "Tune down / up by one step"},
            {"M", "Mute"},
            {SHARKSDR_CTRL_KEY "+S", "Save settings"},
            {SHARKSDR_CTRL_KEY "+Q", "Quit"},
            {"Click / drag on spectrum", "Tune the VFO"},
            {"Drag VFO band edge", "Change the bandwidth"},
            {"Mouse wheel on spectrum", "Tune by one step"},
            {SHARKSDR_CTRL_KEY " + mouse wheel", "Zoom around the cursor"},
            {"Right / middle drag", "Pan the zoomed view"},
            {"Wheel / click on a digit", "Change that digit of the frequency"},
            {"Double-click a slider", "Type a value (also " SHARKSDR_CTRL_KEY "+click; Enter applies, Esc cancels)"},
        };
        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

}  // namespace SharkSDR
