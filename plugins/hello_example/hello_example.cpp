// Minimal example plugin: a sidebar panel, an entry in the "Tools" menu,
// a floating window and an audio tap. Use it as a template.

#include <SharkSDR/plugin.h>
#include <SharkSDR/ui/widgets.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace {

class HelloPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        host_ = &host;
        showWindow_ = host.config().value("showWindow", false);

        // 1) A collapsible panel in the sidebar.
        host.registerPanel("Hello", [this] { drawPanel(); });

        // 2) An item in the Tools menu.
        host.registerMenu([this] { ImGui::MenuItem("Signal history", nullptr, &showWindow_); });

        // 3) A per-frame callback to draw our own window.
        host.registerFrameCallback([this] { drawWindow(); });

        // 4) An audio tap (runs on the DSP thread: keep it short, no locks needed for atomics).
        host.addAudioTap([this](const SharkSDR::Stereo* frames, size_t count, double) {
            float peak = 0.0f;
            for (size_t i = 0; i < count; i++) peak = std::max(peak, std::fabs(frames[i].l));
            audioPeak_ = std::max(peak, audioPeak_.load() * 0.9f);
        });
    }

    void shutdown() override { host_->config()["showWindow"] = showWindow_; }

private:
    void drawPanel() {
        ImGui::Text("Tuned to %s", SharkSDR::ui::formatFrequency(host_->frequency()).c_str());
        ImGui::Text("Mode %s, signal %.1f dBFS", host_->mode().c_str(), host_->signalLevel());
        ImGui::ProgressBar(audioPeak_.load(), ImVec2(-FLT_MIN, 0), "audio");
        if (ImGui::Button("Tune to 100 MHz")) host_->setFrequency(100e6);
        ImGui::SameLine();
        ImGui::Checkbox("History window", &showWindow_);
    }

    void drawWindow() {
        // Record the level once per frame.
        history_.push_back(host_->signalLevel());
        if (history_.size() > 600) history_.erase(history_.begin());
        if (!showWindow_) return;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 40.0f, vp->WorkPos.y + 120.0f), ImGuiCond_FirstUseEver,
                                ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(420, 200), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Signal history", &showWindow_)) {
            ImGui::PlotLines("##hist", history_.data(), int(history_.size()), 0, "signal level (dBFS)", -120.0f, 0.0f,
                             ImGui::GetContentRegionAvail());
        }
        ImGui::End();
    }

    SharkSDR::Host* host_ = nullptr;
    bool showWindow_ = false;
    std::vector<float> history_;
    std::atomic<float> audioPeak_{0.0f};
};

}  // namespace

SHARKSDR_PLUGIN(HelloPlugin, "Hello Example", "1.0.0", "SharkSDR",
                "Template plugin: sidebar panel, Tools menu entry, own window and an audio tap.")
