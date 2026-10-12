#pragma once

#include <SharkSDR/ui/widgets.h>

#include <imgui.h>

namespace SharkSDR::ui {

// Accent colors shared by the application widgets.
namespace colors {
constexpr ImU32 kAccent = IM_COL32(64, 156, 255, 255);
constexpr ImU32 kAccentDim = IM_COL32(64, 156, 255, 60);
constexpr ImU32 kVfoLine = IM_COL32(255, 70, 70, 255);
constexpr ImU32 kVfoBand = IM_COL32(255, 255, 255, 38);
constexpr ImU32 kGrid = IM_COL32(255, 255, 255, 22);
constexpr ImU32 kAxisText = IM_COL32(200, 205, 215, 255);
constexpr ImU32 kSpectrumLine = IM_COL32(110, 190, 255, 255);
constexpr ImU32 kSpectrumFill = IM_COL32(60, 140, 230, 70);
constexpr ImU32 kPlotBackground = IM_COL32(14, 16, 20, 255);
}  // namespace colors

enum class Theme { Dark, Light };

// Applies the color theme and sizes (call once after creating the context).
void applyTheme(Theme theme, float scale);

// Set by the application after loading fonts.
void setFonts(const Fonts& fonts);

}  // namespace SharkSDR::ui
