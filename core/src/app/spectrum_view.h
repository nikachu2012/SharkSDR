#pragma once

#include "app/engine.h"

#include <SharkSDR/host.h>
#include <SharkSDR/ui/texture.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <deque>
#include <functional>
#include <vector>

namespace SharkSDR {

using OverlayFn = std::function<void(const SpectrumOverlayContext&)>;

// FFT plot + waterfall with VFO interaction (the main area of the window).
class SpectrumView {
public:
    explicit SpectrumView(Engine& engine);

    void loadState(const nlohmann::json& j);
    void saveState(nlohmann::json& j) const;

    // Pulls new FFT frames from the engine (call once per frame).
    void update();

    // Draws into the current window at the cursor position.
    void draw(const ImVec2& size, const std::vector<const OverlayFn*>& overlays);

    // Contents of the "Display" sidebar panel.
    void drawSettings();

    void autoRange();

    // Geometry of the last drawn FFT plot (screen coordinates), for tests.
    ImVec2 plotMin() const { return lastPlotMin_; }
    ImVec2 plotMax() const { return lastPlotMax_; }
    ImVec2 maxSliderCenter() const { return lastMaxSliderCenter_; }
    double zoom() const { return zoom_; }
    double viewOffset() const { return viewOffset_; }
    float maxDb() const { return maxDb_; }
    float frequencyToScreenX(double hz) const { return freqToX(hz, lastPlotMin_.x, lastPlotMax_.x); }
    double screenXToFrequency(float x) const { return xToFreq(x, lastPlotMin_.x, lastPlotMax_.x); }

private:
    struct Row {
        std::vector<float> db;
        double rate = 0.0;
        double center = 0.0;
    };
    // Maps each screen column to a range of FFT bins.
    struct ColumnMap {
        std::vector<int> first, last;  // [first, last) ; first < 0 = outside the data
        size_t bins = 0;
        double rate = 0.0, center = 0.0, viewStart = 0.0, viewEnd = 0.0;
        int width = 0;
    };

    double viewSpan() const;
    double viewStart() const;
    double viewEnd() const { return viewStart() + viewSpan(); }
    double maxZoom() const;
    void clampView();
    void followVfo();

    void updateColumns(ColumnMap& map, size_t bins, double rate, double center, int width);
    void columnValues(const std::vector<float>& db, const ColumnMap& map, std::vector<float>& out) const;
    void renderRow(const Row& row, uint32_t* pixels);
    void redrawWaterfall();

    void drawPlot(const ImVec2& min, const ImVec2& max, const std::vector<const OverlayFn*>& overlays);
    void drawAxis(const ImVec2& min, const ImVec2& max);
    void drawWaterfall(const ImVec2& min, const ImVec2& max);
    void drawSliders(const ImVec2& min, const ImVec2& max);
    void drawVfo(ImDrawList* dl, const ImVec2& min, const ImVec2& max, bool waterfall);
    void handleMouse(const ImVec2& min, const ImVec2& max, bool isPlot, bool clickConsumed);
    double frequencyStep(float pixels, float width) const;

    float freqToX(double hz, float x0, float x1) const;
    double xToFreq(float x, float x0, float x1) const;

    Engine& engine_;

    // view
    double zoom_ = 1.0;
    double viewOffset_ = 0.0;  // view center relative to the IQ center
    double lastVfo_ = 0.0;
    float minDb_ = -100.0f;
    float maxDb_ = -20.0f;
    int colormap_ = 0;
    float fftFraction_ = 0.38f;
    float averaging_ = 0.6f;
    bool peakHold_ = false;
    bool fill_ = true;
    bool autoRangeOnStart_ = true;
    bool wasRunning_ = false;
    int autoRangeCountdown_ = -1;  // FFT frames until the automatic range adjustment

    // FFT data
    std::vector<float> smooth_, peak_;
    double lastRate_ = 0.0, lastCenter_ = 0.0;
    std::vector<SpectrumFrame> scratch_;
    ColumnMap plotMap_;
    std::vector<float> plotValues_, peakValues_;
    std::vector<ImVec2> points_;

    // waterfall
    std::deque<Row> rows_;
    ui::Texture texture_;
    int head_ = 0;
    bool waterfallDirty_ = true;
    ColumnMap rowMap_;
    std::vector<uint32_t> pixels_;
    std::vector<float> rowValues_;
    double drawnStart_ = 0.0, drawnEnd_ = 0.0;
    float drawnMin_ = 0.0f, drawnMax_ = 0.0f;
    int drawnColormap_ = -1;

    // interaction
    enum class Drag { None, Tune, BandLow, BandHigh, Pan, Split };
    Drag drag_ = Drag::None;
    double dragOffset_ = 0.0;
    float dragX_ = 0.0f;
    float hoverX_ = -1.0f;
    ImVec2 lastPlotMin_, lastPlotMax_;
    ImVec2 lastMaxSliderCenter_;
};

}  // namespace SharkSDR
