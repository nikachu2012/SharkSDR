#pragma once

#include "app/engine.h"
#include "util/dynlib.h"

#include <SharkSDR/plugin.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace SharkSDR {

class App;
class HostImpl;

struct PluginRecord {
    std::filesystem::path path;
    std::string fileName;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string error;
    bool valid = false;    // library could be inspected and is compatible
    bool notPlugin = false;  // loadable library without the plugin entry points
    bool enabled = true;   // user setting
    bool active = false;   // initialised and running
    OwnerId owner = 0;

    std::unique_ptr<DynamicLibrary> library;
    Plugin* instance = nullptr;
    void (*destroy)(Plugin*) = nullptr;
    std::unique_ptr<HostImpl> host;
};

// Discovers, loads, activates and unloads plugins.
class PluginManager {
public:
    explicit PluginManager(App& app);
    ~PluginManager();

    void scan(const std::vector<std::filesystem::path>& directories);
    void applyEnabled(const nlohmann::json& enabled);  // activates the enabled plugins
    void storeEnabled(nlohmann::json& enabled) const;
    void shutdownAll();

    // Enable/disable at the start of the next frame (safe to call from UI code).
    void requestEnabled(PluginRecord& record, bool enabled);
    void processPending();

    const std::vector<std::unique_ptr<PluginRecord>>& plugins() const { return plugins_; }
    std::vector<std::filesystem::path> directories() const { return directories_; }
    size_t activeCount() const;

private:
    bool openLibrary(PluginRecord& record);
    bool activate(PluginRecord& record);
    void deactivate(PluginRecord& record);

    App& app_;
    std::vector<std::unique_ptr<PluginRecord>> plugins_;
    std::vector<std::filesystem::path> directories_;
    std::vector<std::pair<PluginRecord*, bool>> pending_;
    OwnerId nextOwner_ = 1;
};

}  // namespace SharkSDR
