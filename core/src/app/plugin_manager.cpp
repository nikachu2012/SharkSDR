#include "app/plugin_manager.h"

#include "app/app.h"
#include "app/host_impl.h"

#include <SharkSDR/log.h>
#include <SharkSDR/paths.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace fs = std::filesystem;

namespace SharkSDR {
namespace {

using ApiVersionFn = int (*)();
using SignatureFn = const char* (*)();
using InfoFn = const PluginInfo* (*)();
using CreateFn = Plugin* (*)();
using DestroyFn = void (*)(Plugin*);

bool isPluginFile(const fs::path& p) {
    std::string ext = pathToUtf8(p.extension());
    for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
#if defined(_WIN32)
    return ext == ".dll";
#elif defined(__APPLE__)
    return ext == ".dylib" || ext == ".so";
#else
    return ext == ".so";
#endif
}

}  // namespace

PluginManager::PluginManager(App& app) : app_(app) {}

PluginManager::~PluginManager() { shutdownAll(); }

size_t PluginManager::activeCount() const {
    return size_t(std::count_if(plugins_.begin(), plugins_.end(), [](const auto& p) { return p->active; }));
}

void PluginManager::scan(const std::vector<fs::path>& directories) {
    directories_ = directories;
    for (const auto& dir : directories) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(dir, ec))
            if (entry.is_regular_file(ec) && isPluginFile(entry.path())) files.push_back(entry.path());
        std::sort(files.begin(), files.end());

        for (const auto& file : files) {
            auto rec = std::make_unique<PluginRecord>();
            rec->path = file;
            rec->fileName = pathToUtf8(file.filename());
            const bool ok = openLibrary(*rec);
            if (!ok && rec->notPlugin) {
                // Probably a vendor library a plugin depends on (e.g. SoapySDR.dll).
                log(LogLevel::Debug, "Plugins", rec->fileName + " is not a plugin, skipped");
                continue;
            }
            if (ok) {
                for (const auto& other : plugins_) {
                    if (other->valid && other->name == rec->name) {
                        rec->valid = false;
                        rec->error = "Duplicate of " + pathToUtf8(other->path);
                        rec->library.reset();
                        break;
                    }
                }
            }
            if (!rec->error.empty()) logWarning("Plugins", rec->fileName + ": " + rec->error);
            plugins_.push_back(std::move(rec));
        }
    }
    logInfo("Plugins", "Found " + std::to_string(plugins_.size()) + " plugin file(s)");
}

bool PluginManager::openLibrary(PluginRecord& rec) {
    rec.library = std::make_unique<DynamicLibrary>();
    std::string err;
    if (!rec.library->open(rec.path, &err)) {
        rec.error = "Cannot load library: " + err;
        rec.library.reset();
        return false;
    }
    auto apiVersion = rec.library->function<ApiVersionFn>("sharksdr_plugin_api_version");
    auto signature = rec.library->function<SignatureFn>("sharksdr_plugin_build_signature");
    auto info = rec.library->function<InfoFn>("sharksdr_plugin_info");
    auto create = rec.library->function<CreateFn>("sharksdr_plugin_create");
    auto destroy = rec.library->function<DestroyFn>("sharksdr_plugin_destroy");
    if (!apiVersion || !signature || !info || !create || !destroy) {
        rec.error = "Not a SharkSDR plugin (entry points missing)";
        rec.notPlugin = true;
        rec.library.reset();
        return false;
    }
    if (apiVersion() != kPluginApiVersion) {
        rec.error = "Plugin API version " + std::to_string(apiVersion()) + " (expected " +
                    std::to_string(kPluginApiVersion) + ")";
        rec.library.reset();
        return false;
    }
    if (std::strcmp(signature(), SHARKSDR_BUILD_SIGNATURE) != 0) {
        rec.error = std::string("Built for ") + signature() + ", application is " + SHARKSDR_BUILD_SIGNATURE;
        rec.library.reset();
        return false;
    }
    const PluginInfo* pi = info();
    rec.name = pi && pi->name ? pi->name : rec.fileName;
    rec.version = pi && pi->version ? pi->version : "";
    rec.author = pi && pi->author ? pi->author : "";
    rec.description = pi && pi->description ? pi->description : "";
    rec.destroy = destroy;
    rec.valid = true;
    rec.error.clear();
    return true;
}

bool PluginManager::activate(PluginRecord& rec) {
    if (rec.active || !rec.valid) return rec.active;
    if (!rec.library && !openLibrary(rec)) return false;
    auto create = rec.library->function<CreateFn>("sharksdr_plugin_create");
    rec.destroy = rec.library->function<DestroyFn>("sharksdr_plugin_destroy");
    rec.owner = nextOwner_++;
    try {
        rec.instance = create();
        if (!rec.instance) throw std::runtime_error("create() returned null");
        rec.host = std::make_unique<HostImpl>(app_, rec.owner, rec.name);
        rec.instance->init(*rec.host);
    } catch (const std::exception& e) {
        rec.error = std::string("init failed: ") + e.what();
        logError("Plugins", rec.name + ": " + rec.error);
        app_.removeOwner(rec.owner);
        if (rec.instance) rec.destroy(rec.instance);
        rec.instance = nullptr;
        rec.host.reset();
        return false;
    }
    rec.active = true;
    rec.error.clear();
    logInfo("Plugins", "Loaded " + rec.name + " " + rec.version);
    return true;
}

void PluginManager::deactivate(PluginRecord& rec) {
    if (!rec.active) return;
    app_.removeOwner(rec.owner);
    try {
        rec.instance->shutdown();
    } catch (const std::exception& e) {
        logError("Plugins", rec.name + ": shutdown failed: " + e.what());
    }
    rec.destroy(rec.instance);
    rec.instance = nullptr;
    rec.host.reset();
    rec.active = false;
    rec.library.reset();  // unload the library
    logInfo("Plugins", "Unloaded " + rec.name);
}

void PluginManager::applyEnabled(const nlohmann::json& enabled) {
    for (auto& rec : plugins_) {
        if (!rec->valid) continue;
        rec->enabled = enabled.is_object() ? enabled.value(rec->name, true) : true;
        if (rec->enabled) activate(*rec);
        else rec->library.reset();
    }
}

void PluginManager::storeEnabled(nlohmann::json& enabled) const {
    if (!enabled.is_object()) enabled = nlohmann::json::object();
    for (const auto& rec : plugins_)
        if (rec->valid) enabled[rec->name] = rec->enabled;
}

void PluginManager::requestEnabled(PluginRecord& record, bool enabled) { pending_.emplace_back(&record, enabled); }

void PluginManager::processPending() {
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto& [rec, enable] : pending) {
        rec->enabled = enable;
        if (enable) activate(*rec);
        else deactivate(*rec);
    }
}

void PluginManager::shutdownAll() {
    // Reverse order of loading.
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) deactivate(**it);
}

}  // namespace SharkSDR
