#include "app/spectrum_view.h"

#include "app/colormap.h"
#include "ui/theme.h"

#include <SharkSDR/ui/widgets.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace SharkSDR {
namespace {

constexpr size_t kMaxRowBins = 8192;
constexpr size_t kMaxRows = 4096;
constexpr uint32_t kWaterfallBackground = IM_COL32(8, 10, 14, 255);
// Marks screen columns without FFT data (NaN is unreliable with fast-math).
constexpr float kNoData = -1e30f;
inline bool noData(float v) { return v < -1e29f; }

double snapTo(double f, double step) { return step > 0.0 ? std::round(f / step) * step : f; }

int decimalsFor(double stepMHz) {
    int d = 0;
    double v = stepMHz;
    while (d < 6 && std::fabs(v - std::round(v)) > 1e-6) {
        v *= 10.0;
        d++;
    }
    return d;
}

}  // namespace

SpectrumView::SpectrumView(Engine& engine) : engine_(engine) {}

// ---- persistence ------------------------------------------------------------------

void SpectrumView::loadState(const nlohmann::json& j) {
    if (!j.is_object()) return;
    minDb_ = j.value("minDb", minDb_);
    maxDb_ = j.value("maxDb", maxDb_);
    colormap_ = std::clamp(j.value("colormap", colormap_), 0, int(colormaps().size()) - 1);
    fftFraction_ = std::clamp(j.value("fftFraction", fftFraction_), 0.1f, 0.9f);
    averaging_ = std::clamp(j.value("averaging", averaging_), 0.0f, 0.98f);
    peakHold_ = j.value("peakHold", peakHold_);
    fill_ = j.value("fill", fill_);
    autoRangeOnStart_ = j.value("autoRangeOnStart", autoRangeOnStart_);
    zoom_ = std::max(1.0, j.value("zoom", zoom_));
    viewOffset_ = j.value("viewOffset", viewOffset_);
}

void SpectrumView::saveState(nlohmann::json& j) const {
    j["minDb"] = minDb_;
    j["maxDb"] = maxDb_;
    j["colormap"] = colormap_;
    j["fftFraction"] = fftFraction_;
    j["averaging"] = averaging_;
    j["peakHold"] = peakHold_;
    j["fill"] = fill_;
    j["autoRangeOnStart"] = autoRangeOnStart_;
    j["zoom"] = zoom_;
    j["viewOffset"] = viewOffset_;
}

// ---- view geometry ----------------------------------------------------------------

// The usable part of the band (without the source's filter edges), all of
// which is shown at zoom 1.
double SpectrumView::bandSpan() const {
    double rate = engine_.sampleRate();
    if (rate <= 0.0) rate = lastRate_ > 0.0 ? lastRate_ : 2.4e6;
    return rate * engine_.usableSpectrumRatio();
}

double SpectrumView::viewSpan() const { return bandSpan() / zoom_; }

double SpectrumView::viewStart() const { return engine_.centerFrequency() + viewOffset_ - viewSpan() * 0.5; }

double SpectrumView::maxZoom() const {
    const double bins = smooth_.empty() ? double(engine_.spectrumSettings().fftSize) : double(smooth_.size());
    return std::max(1.0, bins / 32.0);
}

void SpectrumView::clampView() {
    zoom_ = std::clamp(zoom_, 1.0, maxZoom());
    const double band = bandSpan();
    const double limit = (band - band / zoom_) * 0.5;
    viewOffset_ = std::clamp(viewOffset_, -limit, limit);
}

void SpectrumView::followVfo() {
    const double vfo = engine_.frequency();
    if (vfo == lastVfo_) return;
    lastVfo_ = vfo;
    if (drag_ == Drag::Pan) return;
    const double start = viewStart(), end = viewEnd();
    const double margin = viewSpan() * 0.05;
    if (vfo < start + margin) viewOffset_ -= (start + margin) - vfo;
    else if (vfo > end - margin) viewOffset_ += vfo - (end - margin);
    clampView();
}

float SpectrumView::freqToX(double hz, float x0, float x1) const {
    return x0 + float((hz - viewStart()) / viewSpan()) * (x1 - x0);
}

double SpectrumView::xToFreq(float x, float x0, float x1) const {
    return viewStart() + double(x - x0) / double(x1 - x0) * viewSpan();
}

double SpectrumView::frequencyStep(float pixels, float width) const {
    const double raw = viewSpan() * pixels / std::max(1.0f, width);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (m * mag >= raw) return m * mag;
    return 10.0 * mag;
}

// ---- data -------------------------------------------------------------------------

void SpectrumView::update() {
    scratch_.clear();
    engine_.takeSpectrumFrames(scratch_);

    // Fit the dB range to the signal ~1.5 s after the radio was started.
    const bool running = engine_.running();
    if (running && !wasRunning_ && autoRangeOnStart_) autoRangeCountdown_ = 45;
    wasRunning_ = running;
    if (autoRangeCountdown_ > 0 && !scratch_.empty()) {
        autoRangeCountdown_ -= int(scratch_.size());
        if (autoRangeCountdown_ <= 0) autoRange();
    }

    // A changed view/range/colormap requires a full waterfall redraw.
    if (viewStart() != drawnStart_ || viewEnd() != drawnEnd_ || minDb_ != drawnMin_ || maxDb_ != drawnMax_ ||
        colormap_ != drawnColormap_)
        waterfallDirty_ = true;

    for (auto& f : scratch_) {
        const size_t n = f.db.size();
        if (n == 0) continue;
        const bool reset = smooth_.size() != n || f.sampleRate != lastRate_ || f.centerFrequency != lastCenter_;
        if (reset) {
            smooth_ = f.db;
            peak_ = f.db;
        } else {
            const float a = averaging_, b = 1.0f - averaging_;
            for (size_t i = 0; i < n; i++) {
                smooth_[i] = smooth_[i] * a + f.db[i] * b;
                peak_[i] = std::max(peak_[i] - 0.05f, smooth_[i]);
            }
        }
        lastRate_ = f.sampleRate;
        lastCenter_ = f.centerFrequency;

        // Store a (max-pooled) copy for the waterfall history.
        Row row;
        row.rate = f.sampleRate;
        row.center = f.centerFrequency;
        if (n <= kMaxRowBins) {
            row.db = std::move(f.db);
        } else {
            const size_t factor = n / kMaxRowBins;
            row.db.resize(n / factor);
            for (size_t i = 0; i < row.db.size(); i++) {
                float m = f.db[i * factor];
                for (size_t k = 1; k < factor; k++) m = std::max(m, f.db[i * factor + k]);
                row.db[i] = m;
            }
        }
        rows_.push_front(std::move(row));
        const size_t limit = texture_.valid() ? size_t(texture_.height()) : kMaxRows;
        while (rows_.size() > limit) rows_.pop_back();

        if (texture_.valid() && !waterfallDirty_) {
            const int h = texture_.height();
            head_ = (head_ - 1 + h) % h;
            pixels_.resize(size_t(texture_.width()));
            renderRow(rows_.front(), pixels_.data());
            texture_.updateRows(head_, 1, pixels_.data());
        }
    }
}

void SpectrumView::updateColumns(ColumnMap& map, size_t bins, double rate, double center, int width) {
    const double vs = viewStart(), ve = viewEnd();
    if (map.bins == bins && map.rate == rate && map.center == center && map.viewStart == vs && map.viewEnd == ve &&
        map.width == width)
        return;
    map.bins = bins;
    map.rate = rate;
    map.center = center;
    map.viewStart = vs;
    map.viewEnd = ve;
    map.width = width;
    map.first.resize(size_t(width));
    map.last.resize(size_t(width));
    const double binW = rate / double(bins);
    const double low = center - rate * 0.5;
    const double fpp = (ve - vs) / double(width);
    for (int x = 0; x < width; x++) {
        const double a = (vs + x * fpp - low) / binW;
        const double b = (vs + (x + 1) * fpp - low) / binW;
        int i0 = int(std::floor(a));
        int i1 = int(std::ceil(b));
        if (i1 <= 0 || i0 >= int(bins)) {
            map.first[size_t(x)] = -1;
            map.last[size_t(x)] = -1;
            continue;
        }
        i0 = std::max(i0, 0);
        i1 = std::min(i1, int(bins));
        if (i1 <= i0) i1 = std::min(i0 + 1, int(bins));
        map.first[size_t(x)] = i0;
        map.last[size_t(x)] = i1;
    }
}

void SpectrumView::columnValues(const std::vector<float>& db, const ColumnMap& map, std::vector<float>& out) const {
    out.resize(map.first.size());
    for (size_t x = 0; x < out.size(); x++) {
        const int a = map.first[x], b = map.last[x];
        if (a < 0) {
            out[x] = kNoData;
            continue;
        }
        float m = db[size_t(a)];
        for (int i = a + 1; i < b; i++) m = std::max(m, db[size_t(i)]);
        out[x] = m;
    }
}

void SpectrumView::renderRow(const Row& row, uint32_t* pixels) {
    const int w = texture_.width();
    updateColumns(rowMap_, row.db.size(), row.rate, row.center, w);
    columnValues(row.db, rowMap_, rowValues_);
    const auto& lut = colormaps()[size_t(colormap_)].lut;
    const float scale = 255.0f / std::max(1.0f, maxDb_ - minDb_);
    for (int x = 0; x < w; x++) {
        const float v = rowValues_[size_t(x)];
        if (noData(v)) {
            pixels[x] = kWaterfallBackground;
            continue;
        }
        const int idx = std::clamp(int((v - minDb_) * scale), 0, 255);
        pixels[x] = lut[size_t(idx)];
    }
}

void SpectrumView::redrawWaterfall() {
    if (!texture_.valid()) return;
    const int w = texture_.width(), h = texture_.height();
    pixels_.assign(size_t(w) * size_t(h), kWaterfallBackground);
    head_ = 0;
    const size_t rows = std::min(rows_.size(), size_t(h));
    for (size_t i = 0; i < rows; i++) renderRow(rows_[i], &pixels_[i * size_t(w)]);
    texture_.updateRows(0, h, pixels_.data());
    drawnStart_ = viewStart();
    drawnEnd_ = viewEnd();
    drawnMin_ = minDb_;
    drawnMax_ = maxDb_;
    drawnColormap_ = colormap_;
    waterfallDirty_ = false;
}

void SpectrumView::autoRange() {
    if (smooth_.empty()) return;
    // Only the usable part: the filter edges would pull the floor down.
    const size_t edge = size_t(double(smooth_.size()) * (1.0 - engine_.usableSpectrumRatio()) * 0.5);
    std::vector<float> v(smooth_.begin() + std::ptrdiff_t(edge), smooth_.end() - std::ptrdiff_t(edge));
    if (v.empty()) return;
    const float peak = *std::max_element(v.begin(), v.end());
    std::nth_element(v.begin(), v.begin() + v.size() / 5, v.end());
    const float floorDb = v[v.size() / 5];
    minDb_ = std::floor(floorDb - 12.0f);
    maxDb_ = std::ceil(std::max(peak + 8.0f, minDb_ + 40.0f));
}

// ---- drawing ----------------------------------------------------------------------

void SpectrumView::draw(const ImVec2& size, const std::vector<const OverlayFn*>& overlays) {
    followVfo();
    clampView();

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float sliderW = ImGui::GetFrameHeight() * 1.3f;
    const float mainW = std::max(50.0f, size.x - sliderW - style.ItemSpacing.x);
    const float axisH = ImGui::GetFontSize() + 8.0f;
    const float splitH = 6.0f;
    const float avail = std::max(60.0f, size.y - axisH - splitH);
    const float plotH = std::floor(std::max(30.0f, avail * fftFraction_));
    const float wfH = std::max(10.0f, avail - plotH);

    const ImVec2 plotMin = origin;
    const ImVec2 plotMax(origin.x + mainW, origin.y + plotH);
    const ImVec2 axisMin(origin.x, plotMax.y);
    const ImVec2 axisMax(plotMax.x, plotMax.y + axisH);
    const ImVec2 splitMin(origin.x, axisMax.y);
    const ImVec2 wfMin(origin.x, axisMax.y + splitH);
    const ImVec2 wfMax(plotMax.x, wfMin.y + wfH);

    // Waterfall texture follows the widget size (1 texel per point).
    const int tw = std::max(1, int(mainW));
    const int th = std::max(1, int(wfH));
    if (!texture_.valid() || texture_.width() != tw || texture_.height() != th) {
        texture_.create(tw, th, true);
        while (rows_.size() > size_t(th)) rows_.pop_back();
        waterfallDirty_ = true;
    }
    if (viewStart() != drawnStart_ || viewEnd() != drawnEnd_ || minDb_ != drawnMin_ || maxDb_ != drawnMax_ ||
        colormap_ != drawnColormap_)
        waterfallDirty_ = true;
    if (waterfallDirty_) redrawWaterfall();

    ImGui::PushID("spectrum");
    drawPlot(plotMin, plotMax, overlays);
    drawAxis(axisMin, axisMax);

    // Splitter between the FFT and the waterfall.
    ImGui::SetCursorScreenPos(splitMin);
    ImGui::InvisibleButton("##split", ImVec2(mainW, splitH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) fftFraction_ = std::clamp(fftFraction_ + ImGui::GetIO().MouseDelta.y / avail, 0.1f, 0.9f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(splitMin.x, splitMin.y + splitH * 0.5f),
                                        ImVec2(splitMin.x + mainW, splitMin.y + splitH * 0.5f),
                                        ImGui::IsItemHovered() || ImGui::IsItemActive() ? ui::colors::kAccent
                                                                                        : IM_COL32(255, 255, 255, 25));

    drawWaterfall(wfMin, wfMax);
    drawSliders(ImVec2(plotMax.x + style.ItemSpacing.x, origin.y), ImVec2(origin.x + size.x, origin.y + size.y));
    ImGui::PopID();

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(size);
}

void SpectrumView::drawPlot(const ImVec2& min, const ImVec2& max, const std::vector<const OverlayFn*>& overlays) {
    lastPlotMin_ = min;
    lastPlotMax_ = max;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = max.x - min.x, h = max.y - min.y;
    dl->AddRectFilled(min, max, ui::colors::kPlotBackground);
    dl->PushClipRect(min, max, true);

    // Horizontal grid with dB labels.
    const float range = std::max(1.0f, maxDb_ - minDb_);
    float dbStep = 1.0f;
    for (float s : {1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f}) {
        dbStep = s;
        if (s * h / range >= ImGui::GetFontSize() * 1.8f) break;
    }
    for (float db = std::ceil(minDb_ / dbStep) * dbStep; db <= maxDb_; db += dbStep) {
        const float y = min.y + (maxDb_ - db) / range * h;
        dl->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), ui::colors::kGrid);
        if (y - ImGui::GetFontSize() > min.y) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", db);
            dl->AddText(ImVec2(min.x + 4.0f, y - ImGui::GetFontSize() - 1.0f), IM_COL32(170, 175, 185, 200), buf);
        }
    }

    // Vertical grid at the frequency ticks.
    const double step = frequencyStep(ImGui::GetFontSize() * 7.0f, w);
    for (double f = std::ceil(viewStart() / step) * step; f <= viewEnd(); f += step) {
        const float x = freqToX(f, min.x, max.x);
        dl->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), ui::colors::kGrid);
    }

    // Spectrum trace.
    if (!smooth_.empty() && lastRate_ > 0.0) {
        const int width = std::max(2, int(w));
        updateColumns(plotMap_, smooth_.size(), lastRate_, lastCenter_, width);
        columnValues(smooth_, plotMap_, plotValues_);
        const float xs = w / float(width);
        auto yOf = [&](float v) {
            if (noData(v)) v = minDb_ - 10.0f;
            return std::clamp(min.y + (maxDb_ - v) / range * h, min.y, max.y);
        };
        points_.resize(size_t(width));
        for (int x = 0; x < width; x++) points_[size_t(x)] = ImVec2(min.x + (float(x) + 0.5f) * xs, yOf(plotValues_[size_t(x)]));

        if (fill_) {
            // Vertical gradient under the trace.
            const ImU32 top = ui::colors::kSpectrumFill;
            const ImU32 bottom = IM_COL32(60, 140, 230, 8);
            const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
            const int quads = width - 1;
            dl->PrimReserve(quads * 6, quads * 4);
            for (int x = 0; x < quads; x++) {
                const ImVec2 a = points_[size_t(x)], b = points_[size_t(x) + 1];
                const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
                dl->PrimWriteVtx(a, uv, top);
                dl->PrimWriteVtx(b, uv, top);
                dl->PrimWriteVtx(ImVec2(b.x, max.y), uv, bottom);
                dl->PrimWriteVtx(ImVec2(a.x, max.y), uv, bottom);
                dl->PrimWriteIdx(base);
                dl->PrimWriteIdx(ImDrawIdx(base + 1));
                dl->PrimWriteIdx(ImDrawIdx(base + 2));
                dl->PrimWriteIdx(base);
                dl->PrimWriteIdx(ImDrawIdx(base + 2));
                dl->PrimWriteIdx(ImDrawIdx(base + 3));
            }
        }
        if (peakHold_ && peak_.size() == smooth_.size()) {
            columnValues(peak_, plotMap_, peakValues_);
            std::vector<ImVec2> pk(static_cast<size_t>(width));
            for (int x = 0; x < width; x++) pk[size_t(x)] = ImVec2(points_[size_t(x)].x, yOf(peakValues_[size_t(x)]));
            dl->AddPolyline(pk.data(), width, IM_COL32(255, 200, 80, 140), 1.0f);
        }
        dl->AddPolyline(points_.data(), width, ui::colors::kSpectrumLine, 1.4f);
    } else {
        const char* msg = engine_.running() ? "Waiting for data..." : "Press the Play button to start";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(min.x + (w - ts.x) * 0.5f, min.y + (h - ts.y) * 0.5f), IM_COL32(150, 155, 165, 255), msg);
    }

    drawVfo(dl, min, max, false);

    // Plugin overlays (bookmarks...).
    bool consumed = false;
    SpectrumOverlayContext ctx;
    ctx.drawList = dl;
    ctx.plotMin = min;
    ctx.plotMax = max;
    ctx.viewStartHz = viewStart();
    ctx.viewEndHz = viewEnd();
    ctx.clickConsumed = &consumed;
    for (const OverlayFn* fn : overlays) (*fn)(ctx);

    dl->PopClipRect();

    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##plot", ImVec2(w, h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    handleMouse(min, max, true, consumed);
}

void SpectrumView::drawAxis(const ImVec2& min, const ImVec2& max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = max.x - min.x;
    dl->AddRectFilled(min, max, IM_COL32(20, 22, 27, 255));
    dl->PushClipRect(min, max, true);
    const double step = frequencyStep(ImGui::GetFontSize() * 7.0f, w);
    const int decimals = decimalsFor(step / 1e6);
    for (double f = std::ceil(viewStart() / step) * step; f <= viewEnd(); f += step) {
        const float x = freqToX(f, min.x, max.x);
        dl->AddLine(ImVec2(x, min.y), ImVec2(x, min.y + 4.0f), IM_COL32(200, 205, 215, 160));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, f / 1e6);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        float tx = std::clamp(x - ts.x * 0.5f, min.x + 2.0f, max.x - ts.x - 2.0f);
        dl->AddText(ImVec2(tx, min.y + 4.0f), ui::colors::kAxisText, buf);
    }
    dl->PopClipRect();
}

void SpectrumView::drawWaterfall(const ImVec2& min, const ImVec2& max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (texture_.valid()) {
        // The texture is a ring buffer whose newest row is `head_`. The
        // renderer forces clamp-to-edge sampling, so draw it as two parts
        // split at the wrap point instead of relying on GL_REPEAT.
        const float h = float(texture_.height());
        const float v0 = float(head_) / h;
        const float splitY = min.y + (max.y - min.y) * (1.0f - v0);
        const ImTextureRef tex(texture_.id());
        dl->AddImage(tex, min, ImVec2(max.x, splitY), ImVec2(0.0f, v0), ImVec2(1.0f, 1.0f));
        if (head_ > 0) dl->AddImage(tex, ImVec2(min.x, splitY), max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, v0));
    }
    dl->PushClipRect(min, max, true);
    drawVfo(dl, min, max, true);
    dl->PopClipRect();

    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##waterfall", max - min,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    handleMouse(min, max, false, false);
}

void SpectrumView::drawVfo(ImDrawList* dl, const ImVec2& min, const ImVec2& max, bool waterfall) {
    const double vfo = engine_.frequency();
    const double bw = engine_.bandwidth();
    double lo = vfo - bw * 0.5, hi = vfo + bw * 0.5;
    switch (engine_.bandShape()) {
        case BandShape::Upper: lo = vfo, hi = vfo + bw; break;
        case BandShape::Lower: lo = vfo - bw, hi = vfo; break;
        default: break;
    }
    const float xlo = std::max(min.x, freqToX(lo, min.x, max.x));
    const float xhi = std::min(max.x, freqToX(hi, min.x, max.x));
    if (xhi > xlo) {
        dl->AddRectFilled(ImVec2(xlo, min.y), ImVec2(std::max(xhi, xlo + 1.0f), max.y),
                          waterfall ? IM_COL32(255, 255, 255, 24) : ui::colors::kVfoBand);
    }
    const float xv = freqToX(vfo, min.x, max.x);
    if (xv >= min.x && xv <= max.x) dl->AddLine(ImVec2(xv, min.y), ImVec2(xv, max.y), ui::colors::kVfoLine, 1.5f);
}

void SpectrumView::handleMouse(const ImVec2& min, const ImVec2& max, bool isPlot, bool clickConsumed) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const float w = max.x - min.x;
    const float mx = std::clamp(io.MousePos.x, min.x, max.x);
    const double mf = xToFreq(mx, min.x, max.x);
    const double snap = engine_.snap();

    // VFO band edges (bandwidth can be dragged on the FFT plot).
    const double vfo = engine_.frequency();
    const double bw = engine_.bandwidth();
    const BandShape shape = engine_.bandShape();
    double lo = vfo - bw * 0.5, hi = vfo + bw * 0.5;
    if (shape == BandShape::Upper) lo = vfo, hi = vfo + bw;
    if (shape == BandShape::Lower) lo = vfo - bw, hi = vfo;
    const float xlo = freqToX(lo, min.x, max.x), xhi = freqToX(hi, min.x, max.x);
    const float grab = 5.0f;
    const bool nearLow = isPlot && shape != BandShape::Upper && std::fabs(io.MousePos.x - xlo) < grab;
    const bool nearHigh = isPlot && shape != BandShape::Lower && std::fabs(io.MousePos.x - xhi) < grab && !nearLow;

    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        if (io.MouseWheel != 0.0f) {
            if (io.KeyCtrl) {
                // Zoom around the mouse position.
                const double rel = double(mx - min.x) / double(w);
                zoom_ = std::clamp(zoom_ * (io.MouseWheel > 0.0f ? 1.25 : 0.8), 1.0, maxZoom());
                const double span = viewSpan();
                viewOffset_ = (mf - rel * span + span * 0.5) - engine_.centerFrequency();
                clampView();
            } else {
                const double dir = io.MouseWheel > 0.0f ? 1.0 : -1.0;
                engine_.setFrequency(snapTo(vfo, snap) + dir * snap);
            }
        }
        if (nearLow || nearHigh) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    if (ImGui::IsItemActivated() && !clickConsumed) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (nearLow) drag_ = Drag::BandLow;
            else if (nearHigh) drag_ = Drag::BandHigh;
            else {
                drag_ = Drag::Tune;
                engine_.setFrequency(snapTo(mf, snap));
            }
        } else {
            drag_ = Drag::Pan;
            dragOffset_ = viewOffset_;
            dragX_ = io.MousePos.x;
        }
    }

    if (active) {
        switch (drag_) {
            case Drag::Tune:
                if (io.MouseDelta.x != 0.0f) engine_.setFrequency(snapTo(mf, snap));
                break;
            case Drag::BandLow:
            case Drag::BandHigh: {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                double nbw = shape == BandShape::Center ? 2.0 * std::fabs(mf - vfo)
                             : shape == BandShape::Upper ? mf - vfo
                                                         : vfo - mf;
                engine_.setBandwidth(std::round(std::max(nbw, 1.0)));
                break;
            }
            case Drag::Pan:
                viewOffset_ = dragOffset_ - double(io.MousePos.x - dragX_) * viewSpan() / double(w);
                clampView();
                break;
            default: break;
        }
    }
    if (ImGui::IsItemDeactivated()) drag_ = Drag::None;

    // Cursor line + readout.
    if (hovered || active) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(mx, min.y), ImVec2(mx, max.y), IM_COL32(255, 255, 255, 60));
        if (drag_ == Drag::None) {
            if (isPlot && !plotValues_.empty()) {
                const size_t col = std::min(plotValues_.size() - 1, size_t(std::max(0.0f, (mx - min.x) * float(plotValues_.size()) / w)));
                const float v = plotValues_[col];
                if (noData(v)) ImGui::SetTooltip("%s", ui::formatFrequency(mf).c_str());
                else ImGui::SetTooltip("%s\n%.1f dBFS", ui::formatFrequency(mf).c_str(), v);
            } else {
                ImGui::SetTooltip("%s", ui::formatFrequency(mf).c_str());
            }
        }
    }
}

void SpectrumView::drawSliders(const ImVec2& min, const ImVec2& max) {
    const float colW = max.x - min.x;
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float sh = std::max(20.0f, (max.y - min.y - 3.0f * lineH - 3.0f * spacing) / 3.0f);
    float y = min.y;

    auto label = [&](const char* text) {
        const float tw = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorScreenPos(ImVec2(min.x + std::max(0.0f, (colW - tw) * 0.5f), y));
        ImGui::TextDisabled("%s", text);
        y += lineH;
    };

    label("Zoom");
    ImGui::SetCursorScreenPos(ImVec2(min.x, y));
    float z = float(std::log2(zoom_));
    if (ImGui::VSliderFloat("##zoom", ImVec2(colW, sh), &z, 0.0f, float(std::log2(maxZoom())), "")) {
        zoom_ = std::pow(2.0, double(z));
        viewOffset_ = engine_.frequency() - engine_.centerFrequency();
        clampView();
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetTooltip("Zoom x%.1f", zoom_);
    y += sh + spacing;

    label("Max");
    ImGui::SetCursorScreenPos(ImVec2(min.x, y));
    // A range set by hand cancels the pending automatic adjustment.
    if (ui::VSliderFloat("##max", ImVec2(colW, sh), &maxDb_, -150.0f, 20.0f, "")) {
        minDb_ = std::min(minDb_, maxDb_ - 10.0f);
        autoRangeCountdown_ = -1;
    }
    lastMaxSliderCenter_ = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetTooltip("Max %.0f dB", maxDb_);
    y += sh + spacing;

    label("Min");
    ImGui::SetCursorScreenPos(ImVec2(min.x, y));
    if (ui::VSliderFloat("##min", ImVec2(colW, sh), &minDb_, -160.0f, 10.0f, "")) {
        maxDb_ = std::max(maxDb_, minDb_ + 10.0f);
        autoRangeCountdown_ = -1;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetTooltip("Min %.0f dB", minDb_);
}

// ---- settings panel ---------------------------------------------------------------

void SpectrumView::drawSettings() {
    SpectrumSettings s = engine_.spectrumSettings();
    bool changed = false;

    ui::LabeledRow("FFT size");
    std::string sizeLabel = std::to_string(s.fftSize);
    if (ImGui::BeginCombo("##fftsize", sizeLabel.c_str())) {
        for (int n = 1024; n <= (1 << 18); n <<= 1) {
            if (ImGui::Selectable(std::to_string(n).c_str(), n == s.fftSize)) {
                s.fftSize = n;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    ui::LabeledRow("Window");
    if (ImGui::BeginCombo("##window", dsp::windowName(s.window))) {
        for (int i = 0; i < dsp::kWindowTypeCount; i++) {
            auto t = dsp::WindowType(i);
            if (ImGui::Selectable(dsp::windowName(t), t == s.window)) {
                s.window = t;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    ui::LabeledRow("FFT rate");
    if (ui::SliderFloat("##fftrate", &s.frameRate, 5.0f, 60.0f, "%.0f fps")) changed = true;
    if (changed) engine_.setSpectrumSettings(s);

    ui::LabeledRow("Smoothing");
    ui::SliderFloat("##avg", &averaging_, 0.0f, 0.95f, "%.2f");

    ui::LabeledRow("Colormap");
    if (ImGui::BeginCombo("##colormap", colormaps()[size_t(colormap_)].name.c_str())) {
        for (int i = 0; i < int(colormaps().size()); i++) {
            // Small preview of each map.
            const auto& cm = colormaps()[size_t(i)];
            ImGui::PushID(i);
            if (ImGui::Selectable("##cm", i == colormap_, 0, ImVec2(0, ImGui::GetFontSize()))) colormap_ = i;
            ImGui::SameLine(0, 0);
            const ImVec2 p = ImGui::GetItemRectMin();
            const float pw = ImGui::GetFontSize() * 4.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int k = 0; k < 32; k++) {
                float x0 = p.x + pw * float(k) / 32.0f, x1 = p.x + pw * float(k + 1) / 32.0f;
                dl->AddRectFilled(ImVec2(x0, p.y + 2), ImVec2(x1, p.y + ImGui::GetFontSize() - 2), cm.lut[size_t(k * 8)]);
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x + pw + 8.0f, p.y));
            ImGui::TextUnformatted(cm.name.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    ui::LabeledRow("Range");
    const float autoW = ImGui::CalcTextSize("Auto").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - autoW - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::DragFloatRange2("##range", &minDb_, &maxDb_, 0.5f, -160.0f, 30.0f, "%.0f dB", "%.0f dB"))
        autoRangeCountdown_ = -1;
    ImGui::SameLine();
    if (ImGui::Button("Auto")) autoRange();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fit the range to the noise floor and the strongest signal");

    ImGui::Checkbox("Peak hold", &peakHold_);
    ui::SameLineColumn();
    ImGui::Checkbox("Fill", &fill_);
    ImGui::Checkbox("Auto range on start", &autoRangeOnStart_);
}

}  // namespace SharkSDR
