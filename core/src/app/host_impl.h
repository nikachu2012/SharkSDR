#pragma once

#include "app/engine.h"

#include <SharkSDR/host.h>

#include <string>

namespace SharkSDR {

class App;

// Host implementation handed to one plugin; tags every registration with the
// plugin's owner id so that it can be removed when the plugin is unloaded.
class HostImpl : public Host {
public:
    HostImpl(App& app, OwnerId owner, std::string pluginName);

    void registerSource(std::shared_ptr<Source> source) override;
    void registerDemodulator(const std::string& name, DemodulatorFactory factory) override;
    void registerPanel(const std::string& title, std::function<void()> draw) override;
    void registerFrameCallback(std::function<void()> callback) override;
    void registerMenu(std::function<void()> draw) override;
    void registerSpectrumOverlay(std::function<void(const SpectrumOverlayContext&)> draw) override;

    TapHandle addIqTap(IqTapCallback callback) override;
    TapHandle addAudioTap(AudioTapCallback callback) override;
    void removeTap(TapHandle handle) override;
    TapHandle registerStreamHook(ProcessorType type, IqProcessor hook) override;
    TapHandle registerStreamHook(ProcessorType type, RealProcessor hook) override;
    TapHandle registerStreamHook(ProcessorType type, AudioProcessor hook) override;
    void setBypassDemodulation(bool bypass) override;
    bool bypassDemodulation() const override;

    bool isRunning() const override;
    bool start() override;
    void stop() override;
    std::string sourceName() const override;
    bool sourceTunable() const override;
    double sampleRate() const override;
    double centerFrequency() const override;
    void setCenterFrequency(double hz) override;
    double frequency() const override;
    void setFrequency(double hz) override;
    std::string mode() const override;
    std::vector<std::string> modes() const override;
    bool setMode(const std::string& name) override;
    double bandwidth() const override;
    void setBandwidth(double hz) override;
    float signalLevel() const override;
    bool squelchOpen() const override;

    nlohmann::json& config() override;
    void saveConfig() override;
    std::string dataDirectory() const override;
    void log(LogLevel level, const std::string& message) override;

private:
    App& app_;
    Engine& engine_;
    OwnerId owner_;
    std::string name_;
};

}  // namespace SharkSDR
