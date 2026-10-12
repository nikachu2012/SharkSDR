// Frequency bookmarks: a list of named frequencies (with mode and bandwidth)
// that can be recalled with one click and are shown as markers on the
// spectrum. Demonstrates panels, spectrum overlays and plugin settings.

#include <SharkSDR/plugin.h>
#include <SharkSDR/ui/widgets.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Bookmark {
    std::string name;
    double frequency = 0.0;
    std::string mode;
    double bandwidth = 0.0;
    std::string group;
};

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class Bookmarks {
public:
    explicit Bookmarks(SharkSDR::Host& host) : host_(host) { load(); }

    void drawPanel() {
        // Add the current frequency.
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 4.0f);
        ImGui::InputTextWithHint("##newname", "Name", &newName_);
        ImGui::SameLine();
        if (ImGui::Button("Add", ImVec2(-FLT_MIN, 0))) add();
        if (!newGroup_.empty() || !groups().empty()) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##group", "Group (optional)", &newGroup_);
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "Search", &filter_);
        ImGui::Checkbox("Show on spectrum", &showMarkers_);

        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        const float height = std::min(float(std::max<size_t>(list_.size(), 3)) * rowH + rowH * 1.6f, rowH * 14.0f);
        int removeIndex = -1;
        if (ImGui::BeginTable("##bm", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, height))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Frequency", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.6f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 1.2f);
            ImGui::TableHeadersRow();
            const std::string f = lower(filter_);
            for (size_t i = 0; i < list_.size(); i++) {
                const Bookmark& b = list_[i];
                if (!f.empty() && lower(b.name).find(f) == std::string::npos && lower(b.group).find(f) == std::string::npos)
                    continue;
                ImGui::PushID(int(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool current = std::fabs(host_.frequency() - b.frequency) < 1.0;
                if (ImGui::Selectable(b.name.c_str(), current, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    recall(b);
                if (ImGui::IsItemHovered() && !b.group.empty()) ImGui::SetTooltip("Group: %s", b.group.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(SharkSDR::ui::formatFrequency(b.frequency).c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", b.mode.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("x")) removeIndex = int(i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (removeIndex >= 0) {
            list_.erase(list_.begin() + removeIndex);
            save();
        }
        if (list_.empty()) ImGui::TextDisabled("No bookmarks yet. Tune somewhere and press Add.");
    }

    // Markers along the top of the FFT plot; clicking one recalls it.
    void drawOverlay(const SharkSDR::SpectrumOverlayContext& ctx) {
        if (!showMarkers_) return;
        const ImGuiIO& io = ImGui::GetIO();
        const float pad = 3.0f;
        float lastRight = -1e9f;
        int lane = 0;
        // list_ is kept sorted by frequency, so labels can be stacked in lanes.
        for (const Bookmark& b : list_) {
            if (b.frequency < ctx.viewStartHz || b.frequency > ctx.viewEndHz) continue;
            const float x = ctx.frequencyToX(b.frequency);
            const ImVec2 ts = ImGui::CalcTextSize(b.name.c_str());
            lane = x - ts.x * 0.5f - pad < lastRight + 4.0f ? (lane + 1) % 3 : 0;
            const float y = ctx.plotMin.y + 4.0f + float(lane) * (ts.y + pad * 2.0f + 2.0f);
            const ImVec2 a(x - ts.x * 0.5f - pad, y), c(x + ts.x * 0.5f + pad, y + ts.y + pad * 2.0f);
            const bool hovered = ImGui::IsMouseHoveringRect(a, c) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            ctx.drawList->AddLine(ImVec2(x, c.y), ImVec2(x, ctx.plotMax.y), IM_COL32(255, 210, 90, 70));
            ctx.drawList->AddRectFilled(a, c, hovered ? IM_COL32(255, 210, 90, 230) : IM_COL32(60, 52, 25, 220), 3.0f);
            ctx.drawList->AddRect(a, c, IM_COL32(255, 210, 90, 200), 3.0f);
            ctx.drawList->AddText(ImVec2(a.x + pad, a.y + pad), hovered ? IM_COL32(20, 20, 20, 255) : IM_COL32(255, 230, 160, 255),
                                  b.name.c_str());
            if (hovered) {
                ImGui::SetTooltip("%s\n%s %s", b.name.c_str(), SharkSDR::ui::formatFrequency(b.frequency).c_str(), b.mode.c_str());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    recall(b);
                    *ctx.clickConsumed = true;
                }
            }
            lastRight = c.x;
            (void)io;
        }
    }

    void addCurrent() { add(); }

private:
    std::vector<std::string> groups() const {
        std::vector<std::string> g;
        for (const auto& b : list_)
            if (!b.group.empty() && std::find(g.begin(), g.end(), b.group) == g.end()) g.push_back(b.group);
        return g;
    }

    void add() {
        Bookmark b;
        b.frequency = host_.frequency();
        b.mode = host_.mode();
        b.bandwidth = host_.bandwidth();
        b.group = newGroup_;
        b.name = newName_.empty() ? SharkSDR::ui::formatFrequency(b.frequency) : newName_;
        list_.push_back(b);
        sort();
        newName_.clear();
        save();
    }

    void recall(const Bookmark& b) {
        if (!b.mode.empty()) host_.setMode(b.mode);
        if (b.bandwidth > 0) host_.setBandwidth(b.bandwidth);
        host_.setFrequency(b.frequency);
    }

    void sort() {
        std::sort(list_.begin(), list_.end(), [](const Bookmark& a, const Bookmark& b) { return a.frequency < b.frequency; });
    }

    void load() {
        const auto& c = host_.config();
        showMarkers_ = c.value("showMarkers", true);
        if (c.contains("bookmarks") && c["bookmarks"].is_array()) {
            for (const auto& j : c["bookmarks"]) {
                Bookmark b;
                b.name = j.value("name", std::string());
                b.frequency = j.value("frequency", 0.0);
                b.mode = j.value("mode", std::string());
                b.bandwidth = j.value("bandwidth", 0.0);
                b.group = j.value("group", std::string());
                list_.push_back(b);
            }
        } else {
            // A few examples for the demo signal generator.
            list_ = {{"Demo WFM", 99.7e6, "WFM", 200000.0, "Demo"},
                     {"Demo NFM", 100.15e6, "NFM", 12500.0, "Demo"},
                     {"Demo AM", 100.3e6, "AM", 10000.0, "Demo"},
                     {"Demo CW", 100.5e6, "CW", 500.0, "Demo"}};
        }
        sort();
    }

    void save() {
        auto& c = host_.config();
        c["showMarkers"] = showMarkers_;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& b : list_)
            arr.push_back({{"name", b.name}, {"frequency", b.frequency}, {"mode", b.mode}, {"bandwidth", b.bandwidth}, {"group", b.group}});
        c["bookmarks"] = arr;
    }

    SharkSDR::Host& host_;
    std::vector<Bookmark> list_;
    std::string newName_;
    std::string newGroup_;
    std::string filter_;
    bool showMarkers_ = true;
};

class BookmarksPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        bookmarks_ = std::make_unique<Bookmarks>(host);
        host.registerPanel("Bookmarks", [this] { bookmarks_->drawPanel(); });
        host.registerSpectrumOverlay([this](const SharkSDR::SpectrumOverlayContext& ctx) { bookmarks_->drawOverlay(ctx); });
        host.registerMenu([this] {
            if (ImGui::MenuItem("Bookmark current frequency")) bookmarks_->addCurrent();
        });
    }
    void shutdown() override { bookmarks_.reset(); }

private:
    std::unique_ptr<Bookmarks> bookmarks_;
};

}  // namespace

SHARKSDR_PLUGIN(BookmarksPlugin, "Bookmarks", "1.0.0", "SharkSDR",
                "Named frequency list with one-click recall and markers on the spectrum.")
