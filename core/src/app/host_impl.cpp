#include "app/host_impl.h"

#include "app/app.h"

#include <SharkSDR/paths.h>

namespace SharkSDR {

HostImpl::HostImpl(App& app, OwnerId owner, std::string pluginName)
    : app_(app), engine_(app.engine()), owner_(owner), name_(std::move(pluginName)) {}

void HostImpl::registerSource(std::shared_ptr<Source> source) { engine_.addSource(std::move(source), owner_); }

void HostImpl::registerDemodulator(const std::string& name, DemodulatorFactory factory) {
    engine_.registerDemodulator(name, std::move(factory), owner_);
}

void HostImpl::registerPanel(const std::string& title, std::function<void()> draw) {
    app_.addPanel(owner_, title, std::move(draw));
}

void HostImpl::registerFrameCallback(std::function<void()> callback) {
    app_.addFrameCallback(owner_, std::move(callback));
}

void HostImpl::registerMenu(std::function<void()> draw) { app_.addMenu(owner_, std::move(draw)); }

void HostImpl::registerSpectrumOverlay(std::function<void(const SpectrumOverlayContext&)> draw) {
    app_.addOverlay(owner_, std::move(draw));
}

TapHandle HostImpl::addIqTap(IqTapCallback callback) { return engine_.addIqTap(std::move(callback), owner_); }
TapHandle HostImpl::addAudioTap(AudioTapCallback callback) { return engine_.addAudioTap(std::move(callback), owner_); }
void HostImpl::removeTap(TapHandle handle) { engine_.removeTap(handle); }

bool HostImpl::isRunning() const { return engine_.running(); }
bool HostImpl::start() { return engine_.start(); }
void HostImpl::stop() { engine_.stop(); }
std::string HostImpl::sourceName() const { return engine_.currentSourceName(); }
double HostImpl::sampleRate() const { return engine_.sampleRate(); }
double HostImpl::centerFrequency() const { return engine_.centerFrequency(); }
double HostImpl::frequency() const { return engine_.frequency(); }
void HostImpl::setFrequency(double hz) { engine_.setFrequency(hz); }
std::string HostImpl::mode() const { return engine_.mode(); }
std::vector<std::string> HostImpl::modes() const { return engine_.modes(); }
bool HostImpl::setMode(const std::string& name) { return engine_.setMode(name); }
double HostImpl::bandwidth() const { return engine_.bandwidth(); }
void HostImpl::setBandwidth(double hz) { engine_.setBandwidth(hz); }
float HostImpl::signalLevel() const { return engine_.signalLevel(); }
bool HostImpl::squelchOpen() const { return engine_.squelchOpen(); }

nlohmann::json& HostImpl::config() { return app_.pluginSettings(name_); }
void HostImpl::saveConfig() { app_.saveConfig(); }
std::string HostImpl::dataDirectory() const { return pathToUtf8(userDataDirectory()); }
void HostImpl::log(LogLevel level, const std::string& message) { SharkSDR::log(level, name_, message); }

}  // namespace SharkSDR
