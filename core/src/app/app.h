#pragma once

#include "app/audio_output.h"
#include "app/engine.h"
#include "app/spectrum_view.h"
#include "ui/theme.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace SharkSDR {

class PluginManager;

// The application: window, main loop, layout and the registries that plugins
// extend through their Host.
class App {
public:
    App();
    ~App();

    int run(int argc, char** argv);

    // ---- used by HostImpl ----------------------------------------------------
    Engine& engine() { return *engine_; }
    nlohmann::json& pluginSettings(const std::string& pluginName);
    void saveConfig();
    void addPanel(OwnerId owner, const std::string& title, std::function<void()> draw);
    void addFrameCallback(OwnerId owner, std::function<void()> fn);
    void addMenu(OwnerId owner, std::function<void()> fn);
    void addOverlay(OwnerId owner, OverlayFn fn);
    // Removes everything registered by `owner` (UI elements, sources, modes, taps).
    void removeOwner(OwnerId owner);

private:
    struct Options {
        std::filesystem::path configDir;
        std::vector<std::filesystem::path> pluginDirs;
        bool autostart = false;
        bool noPlugins = false;
        std::filesystem::path screenshot;
        int screenshotFrames = 120;
        bool pluginReloadTest = false;  // developer option: unload/reload all plugins while running
        bool uiTest = false;            // developer option: scripted mouse interaction test
    };
    struct Panel {
        OwnerId owner;
        std::string title;
        std::function<void()> draw;
    };
    struct Callback {
        OwnerId owner;
        std::function<void()> fn;
    };
    struct Overlay {
        OwnerId owner;
        OverlayFn fn;
    };

    bool parseArguments(int argc, char** argv);
    void loadConfig();
    void storeState();
    bool initWindow();
    void initImGui();
    void loadFonts();
    void shutdown();
    void captureScreenshot(const std::filesystem::path& path);

    // UI
    void drawFrame();
    void drawMenuBar();
    void drawToolbar(float height);
    void drawSidebar();
    void drawStatusBar(float height);
    bool beginPanel(const char* title, bool defaultOpen = true);
    void drawSourcePanel();
    void drawRadioPanel();
    void drawAudioPanel();
    void drawDisplayPanel();
    void drawPluginsWindow();
    void drawLogWindow();
    void drawAboutWindow();
    void drawShortcutsWindow();
    void handleShortcuts();
    void setTheme(ui::Theme theme);
    void tuneStep(int direction);
    void updateWindowTitle();
    void uiTestStep();

    Options options_;
    GLFWwindow* window_ = nullptr;
    nlohmann::json config_ = nlohmann::json::object();
    std::filesystem::path configPath_;
    std::string iniPath_;

    std::unique_ptr<AudioOutput> audio_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<SpectrumView> spectrum_;
    std::unique_ptr<PluginManager> plugins_;

    std::vector<Panel> panels_;
    std::vector<Callback> frameCallbacks_;
    std::vector<Callback> menus_;
    std::vector<Overlay> overlays_;

    // UI state
    float dpiScale_ = 1.0f;
    float uiScale_ = 1.0f;
    ui::Theme theme_ = ui::Theme::Dark;
    float sidebarWidth_ = 300.0f;
    bool showSidebar_ = true;
    bool showStatusBar_ = true;
    bool showPlugins_ = false;
    bool showLog_ = false;
    bool showAbout_ = false;
    bool showShortcuts_ = false;
    bool showDemo_ = false;
    bool startOnLaunch_ = false;
    nlohmann::json panelOpen_ = nlohmann::json::object();
    uint64_t logCursor_ = 0;
    std::vector<LogEntry> logLines_;
    int frameCount_ = 0;
    std::chrono::steady_clock::time_point lastSave_;
    std::string windowTitle_;
    float uiTestX_ = 0.0f;
    double uiTestValue_ = 0.0;
    double uiTestValue2_ = 0.0;
    double uiTestStartFreq_ = 0.0;
    double uiTestStartBw_ = 0.0;
    int uiTestFailures_ = 0;
    ImVec2 uiTestVolumeMin_, uiTestVolumeMax_;  // toolbar volume slider
};

}  // namespace SharkSDR
