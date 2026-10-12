#pragma once

#include <SharkSDR/export.h>

#include <imgui.h>

#include <string>

// Reusable ImGui widgets. Plugins are encouraged to use them so that the
// whole application looks consistent.
namespace SharkSDR::ui {

struct Fonts {
    ImFont* regular = nullptr;  // UI font
    ImFont* mono = nullptr;     // monospace font
};
SHARKSDR_API const Fonts& fonts();

// Big frequency display (like a radio dial). Each digit can be changed with
// the mouse wheel, by clicking its upper/lower half, or with the arrow keys
// while hovered. Returns true when the value changed.
SHARKSDR_API bool FrequencyDial(const char* id, double* hz, double minHz, double maxHz, float fontSize);

// Text field accepting "100.5M", "7100k", "145500000"... Returns true on Enter.
SHARKSDR_API bool InputFrequency(const char* label, double* hz);

// Button that stays highlighted when `active` is true.
SHARKSDR_API bool ToggleButton(const char* label, bool active, const ImVec2& size = ImVec2(0, 0));

// "(?)" marker with a tooltip.
SHARKSDR_API void HelpMarker(const char* text);

// Text label on the left and a widget taking the remaining width:
//     if (ui::LabeledRow("Gain")) ui::SliderFloat("##gain", ...);
// Always returns true; it only sets up the layout. An empty label puts the
// widget in the value column under the widgets of the rows above.
SHARKSDR_API bool LabeledRow(const char* label, float labelWidth = 0.0f);

// Same as LabeledRow() with a checkbox in front of the label, for settings
// that can be switched off:
//     ui::CheckboxRow("Squelch", &on); ui::SliderFloat("##level", ...);
// Returns true when the checkbox was toggled.
SHARKSDR_API bool CheckboxRow(const char* label, bool* value, float labelWidth = 0.0f);

// Width of the label column used by LabeledRow() / CheckboxRow().
SHARKSDR_API float LabelWidth();

// Puts the next widget in the right half of the current row, so that pairs of
// checkboxes / radio buttons line up from one row to the next:
//     ImGui::Checkbox("Peak hold", &a); ui::SameLineColumn(); ImGui::Checkbox("Fill", &b);
// When the previous widget is too wide the next one simply follows it.
SHARKSDR_API void SameLineColumn();

// Sliders that also accept typed values: double-click (or Ctrl+click) turns the
// slider into a text field; Enter applies, Esc cancels. Units and SI prefixes
// may be typed ("12.5k", "1.2 MHz", "-40 dB"), the result is clamped to
// [min, max]. Same parameters as the ImGui functions.
SHARKSDR_API bool SliderFloat(const char* label, float* value, float min, float max, const char* format = "%.3f",
                              ImGuiSliderFlags flags = 0);
SHARKSDR_API bool SliderInt(const char* label, int* value, int min, int max, const char* format = "%d",
                            ImGuiSliderFlags flags = 0);
// Vertical version: the text field opens in a small popup.
SHARKSDR_API bool VSliderFloat(const char* label, const ImVec2& size, float* value, float min, float max,
                               const char* format = "%.3f", ImGuiSliderFlags flags = 0);

// Horizontal level meter (e.g. signal strength), value in [min, max].
SHARKSDR_API void LevelMeter(const char* id, float value, float min, float max, const ImVec2& size,
                             float marker = -1e30f);

// Formatting / parsing helpers.
SHARKSDR_API std::string formatFrequency(double hz);    // "100.500 000 MHz" style, adaptive unit
SHARKSDR_API std::string formatSampleRate(double sps);  // "2.4 MS/s"
SHARKSDR_API std::string formatBandwidth(double hz);    // "12.5 kHz"
SHARKSDR_API bool parseFrequency(const std::string& text, double* hz);
// Number with an optional SI prefix and unit: "-40 dB", "12.5k", "1.2 MHz", "70 %".
SHARKSDR_API bool parseNumber(const std::string& text, double* value);

// File / folder picker: a path field with a "..." button opening a browser.
//   extensions: e.g. ".wav;.cu8;.cs16" (empty = all files). Returns true when changed.
SHARKSDR_API bool PathPicker(const char* id, std::string* path, bool selectFolder,
                             const char* extensions = "");

}  // namespace SharkSDR::ui
