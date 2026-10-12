#include <SharkSDR/ui/widgets.h>

#include "ui/theme.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <unordered_map>

namespace SharkSDR::ui {

// ---- FrequencyDial ------------------------------------------------------------

bool FrequencyDial(const char* id, double* hz, double minHz, double maxHz, float fontSize) {
    constexpr int kDigits = 12;  // up to 999.999.999.999 Hz
    ImGui::PushID(id);
    ImGui::PushFont(fonts().regular, fontSize);

    float digitW = 0.0f;
    for (char ch = '0'; ch <= '9'; ch++) {
        char s[2] = {ch, 0};
        digitW = std::max(digitW, ImGui::CalcTextSize(s).x);
    }
    const float sepW = ImGui::CalcTextSize(".").x;
    const float h = ImGui::GetFontSize();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(kDigits * digitW + 3 * sepW, h);

    ImGui::InvisibleButton("##dial", size);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);

    int64_t value = int64_t(std::llround(*hz));
    int64_t newValue = value;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.55f);

    bool leading = true;
    float x = pos.x;
    for (int d = 0; d < kDigits; d++) {
        const int power = kDigits - 1 - d;
        int64_t p10 = 1;
        for (int i = 0; i < power; i++) p10 *= 10;
        const int digit = int((value / p10) % 10);
        if (digit != 0 || power == 0) leading = false;

        const ImVec2 rmin(x, pos.y), rmax(x + digitW, pos.y + h);
        if (hovered && ImGui::IsMouseHoveringRect(rmin, rmax)) {
            const bool upper = io.MousePos.y < pos.y + h * 0.5f;
            dl->AddRectFilled(rmin, rmax, colors::kAccentDim, 3.0f);
            dl->AddRectFilled(upper ? rmin : ImVec2(rmin.x, rmin.y + h * 0.5f),
                              upper ? ImVec2(rmax.x, rmin.y + h * 0.5f) : rmax, IM_COL32(255, 255, 255, 18), 3.0f);
            if (io.MouseWheel != 0.0f) newValue += int64_t(io.MouseWheel > 0 ? 1 : -1) * p10;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) newValue += (upper ? 1 : -1) * p10;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) newValue += p10;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) newValue -= p10;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) newValue = (value / p10) * p10;  // zero the digits to the right
            for (int k = 0; k <= 9; k++) {
                if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_0 + k)) || ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_Keypad0 + k)))
                    newValue = value - int64_t(digit) * p10 + int64_t(k) * p10;
            }
        }
        char s[2] = {char('0' + digit), 0};
        float cw = ImGui::CalcTextSize(s).x;
        dl->AddText(ImVec2(x + (digitW - cw) * 0.5f, pos.y), leading ? dimCol : textCol, s);
        x += digitW;
        if (power % 3 == 0 && power != 0) {
            dl->AddText(ImVec2(x, pos.y), leading ? dimCol : textCol, ".");
            x += sepW;
        }
    }

    ImGui::PopFont();
    ImGui::PopID();

    newValue = std::clamp<int64_t>(newValue, int64_t(minHz), int64_t(maxHz));
    if (newValue != value) {
        *hz = double(newValue);
        return true;
    }
    return false;
}

// ---- Simple widgets -------------------------------------------------------------

bool InputFrequency(const char* label, double* hz) {
    struct EditState {
        std::string text;
        bool active = false;
    };
    static std::unordered_map<ImGuiID, EditState> states;
    EditState& st = states[ImGui::GetID(label)];

    // While not being edited the field mirrors the current value.
    if (!st.active) st.text = formatFrequency(*hz);
    const bool enter = ImGui::InputText(label, &st.text,
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    st.active = ImGui::IsItemActive();
    if (!enter) return false;
    double v;
    if (!parseFrequency(st.text, &v)) return false;
    *hz = v;
    return true;
}

bool ToggleButton(const char* label, bool active, const ImVec2& size) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.61f, 1.00f, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.66f, 1.00f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    }
    bool pressed = ImGui::Button(label, size);
    if (active) ImGui::PopStyleColor(3);
    return pressed;
}

void HelpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

float LabelWidth() { return ImGui::GetFontSize() * 6.0f; }

namespace {

// Right edge of the last item, in the coordinates of GetCursorPosX().
float lastItemRight() { return ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX(); }

// Continues a row started at x0 in the value column (or right after a label
// that is too wide for the label column). The next widget fills the row.
// A label may sit closer to its widget than two widgets do (text already
// ends in a little side bearing).
void toValueColumn(float x0, float labelWidth) {
    const float x = x0 + (labelWidth > 0.0f ? labelWidth : LabelWidth());
    if (lastItemRight() + ImGui::GetStyle().ItemSpacing.x * 0.5f <= x) ImGui::SameLine(x);
    else ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

}  // namespace

bool LabeledRow(const char* label, float labelWidth) {
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    toValueColumn(x0, labelWidth);
    return true;
}

bool CheckboxRow(const char* label, bool* value, float labelWidth) {
    const float x0 = ImGui::GetCursorPosX();
    const bool changed = ImGui::Checkbox(label, value);
    toValueColumn(x0, labelWidth);
    return changed;
}

void SameLineColumn() {
    // The cursor is at the start of the next row: the column is half of it.
    const float column = ImGui::GetCursorPosX() + std::floor(ImGui::GetContentRegionAvail().x * 0.5f);
    if (lastItemRight() + ImGui::GetStyle().ItemSpacing.x <= column) ImGui::SameLine(column);
    else ImGui::SameLine();
}

// ---- Sliders with text input ----------------------------------------------------

namespace {

// The slider currently replaced by a text field (only one at a time).
struct SliderEdit {
    ImGuiID id = 0;
    std::string text;
    std::string initial;
    int startFrame = 0;
    int lastFrame = 0;  // last frame the field was drawn
    bool wasActive = false;
};
SliderEdit g_sliderEdit;

// Value before the first click of a double-click (the click moves the slider).
struct SliderPress {
    ImGuiID id = 0;
    double value = 0.0;
};
SliderPress g_sliderPress;

// Text put in the field: the value as the slider shows it ("-50 dB").
std::string sliderText(const char* format, bool integer, double value) {
    char buf[64];
    double parsed;
    if (format && !std::strchr(format, '%') && parseNumber(format, &parsed)) return format;  // e.g. "12.5 kHz"
    if (!format || !std::strchr(format, '%')) std::snprintf(buf, sizeof(buf), "%.6g", value);
    else if (integer) std::snprintf(buf, sizeof(buf), format, int(value));
    else std::snprintf(buf, sizeof(buf), format, value);
    std::string s = buf;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    return s;
}

// Text field of the slider being edited. Returns true when the typed value was applied.
template <typename T>
bool sliderEditField(T* value, T min, T max) {
    SliderEdit& edit = g_sliderEdit;
    const bool enter = ImGui::InputText("##edit", &edit.text,
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool active = ImGui::IsItemActive();
    bool changed = false;
    if (enter || (edit.wasActive && !active)) {  // Enter or click elsewhere applies, Esc cancels
        double v;
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (!cancel && edit.text != edit.initial && parseNumber(edit.text, &v)) {
            v = std::clamp(v, double(std::min(min, max)), double(std::max(min, max)));
            const T nv = std::is_integral_v<T> ? T(std::llround(v)) : T(v);
            changed = nv != *value;
            *value = nv;
        }
        edit.id = 0;
    }
    edit.wasActive = active;
    return changed;
}

// vsize == nullptr: horizontal slider, replaced in place by the text field.
// Otherwise a vertical slider (usually too narrow for text): the field opens in a popup.
template <typename T>
bool sliderWithInput(const char* label, const ImVec2* vsize, T* value, T min, T max, const char* format,
                     ImGuiSliderFlags flags) {
    constexpr bool kInteger = std::is_integral_v<T>;
    const ImGuiID id = ImGui::GetID(label);
    SliderEdit& edit = g_sliderEdit;
    const int frame = ImGui::GetFrameCount();

    if (edit.id == id && (frame - edit.lastFrame > 1 || (!edit.wasActive && frame - edit.startFrame > 10)))
        edit.id = 0;  // the field was hidden or never got the focus: cancel
    if (edit.id == id && !vsize) {
        edit.lastFrame = frame;
        if (frame == edit.startFrame + 1) ImGui::SetKeyboardFocusHere();
        ImGui::PushID(label);
        const bool changed = sliderEditField(value, min, max);
        ImGui::PopID();
        return changed;
    }

    bool changed = false;
    if (vsize) {
        // Popup first, so that the slider stays the last item for the caller.
        ImGui::PushID(label);
        if (edit.id == id && frame == edit.startFrame + 1) ImGui::OpenPopup("##edit");
        if (ImGui::BeginPopup("##edit")) {
            if (edit.id == id) {
                edit.lastFrame = frame;
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                ImGui::SetNextItemWidth(LabelWidth());
                changed = sliderEditField(value, min, max);
            }
            if (edit.id != id) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        } else if (edit.id == id && frame > edit.startFrame + 1) {
            edit.id = 0;  // popup closed by a click outside
        }
        ImGui::PopID();
    }

    const T before = *value;
    const ImGuiSliderFlags sliderFlags = flags | ImGuiSliderFlags_NoInput;  // Ctrl+click is handled below
    if constexpr (kInteger) {
        changed |= vsize ? ImGui::VSliderInt(label, *vsize, value, min, max, format, sliderFlags)
                         : ImGui::SliderInt(label, value, min, max, format, sliderFlags);
    } else {
        changed |= vsize ? ImGui::VSliderFloat(label, *vsize, value, min, max, format, sliderFlags)
                         : ImGui::SliderFloat(label, value, min, max, format, sliderFlags);
    }
    if (flags & ImGuiSliderFlags_NoInput) return changed;

    const bool doubleClick = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool ctrlClick = ImGui::IsItemActivated() && ImGui::GetIO().KeyCtrl;
    if (doubleClick || ctrlClick) {
        // Typing replaces the value: undo the jump caused by the click(s).
        const T restore = doubleClick && g_sliderPress.id == id ? T(g_sliderPress.value) : before;
        *value = restore;
        changed = restore != before;
        edit = SliderEdit();
        edit.id = id;
        edit.startFrame = edit.lastFrame = frame;
        edit.text = edit.initial = sliderText(format, kInteger, double(restore));
    } else if (ImGui::IsItemActivated()) {
        g_sliderPress = {id, double(before)};
    }
    return changed;
}

}  // namespace

bool SliderFloat(const char* label, float* value, float min, float max, const char* format, ImGuiSliderFlags flags) {
    return sliderWithInput(label, nullptr, value, min, max, format, flags);
}

bool SliderInt(const char* label, int* value, int min, int max, const char* format, ImGuiSliderFlags flags) {
    return sliderWithInput(label, nullptr, value, min, max, format, flags);
}

bool VSliderFloat(const char* label, const ImVec2& size, float* value, float min, float max, const char* format,
                  ImGuiSliderFlags flags) {
    return sliderWithInput(label, &size, value, min, max, format, flags);
}

void LevelMeter(const char* id, float value, float min, float max, const ImVec2& sizeArg, float marker) {
    ImVec2 size = sizeArg;
    if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight() * 0.5f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::Dummy(size);
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + size, IM_COL32(30, 33, 38, 255), 2.0f);
    float t = std::clamp((value - min) / (max - min), 0.0f, 1.0f);
    const float w = size.x * t;
    // green -> yellow -> red gradient
    const float x1 = size.x * 0.6f, x2 = size.x * 0.85f;
    auto seg = [&](float a, float b, ImU32 ca, ImU32 cb) {
        if (w <= a) return;
        float e = std::min(w, b);
        dl->AddRectFilledMultiColor(ImVec2(p.x + a, p.y), ImVec2(p.x + e, p.y + size.y), ca, cb, cb, ca);
    };
    seg(0.0f, x1, IM_COL32(40, 180, 90, 255), IM_COL32(120, 210, 70, 255));
    seg(x1, x2, IM_COL32(120, 210, 70, 255), IM_COL32(240, 200, 50, 255));
    seg(x2, size.x, IM_COL32(240, 200, 50, 255), IM_COL32(235, 70, 60, 255));
    for (int i = 1; i < 10; i++) {
        float x = p.x + size.x * float(i) / 10.0f;
        dl->AddLine(ImVec2(x, p.y + size.y * 0.6f), ImVec2(x, p.y + size.y), IM_COL32(0, 0, 0, 120));
    }
    if (marker > min && marker < max) {
        float mx = p.x + size.x * (marker - min) / (max - min);
        dl->AddLine(ImVec2(mx, p.y - 2), ImVec2(mx, p.y + size.y + 2), IM_COL32(255, 255, 255, 220), 2.0f);
    }
}

// ---- Formatting -----------------------------------------------------------------

namespace {
std::string trimZeros(const char* s, int minDecimals) {
    std::string out = s;
    size_t dot = out.find('.');
    if (dot == std::string::npos) return out;
    size_t minLen = dot + 1 + size_t(minDecimals);
    while (out.size() > minLen && out.back() == '0') out.pop_back();
    if (out.back() == '.') out.pop_back();
    return out;
}
}  // namespace

std::string formatFrequency(double hz) {
    char buf[64];
    const double a = std::fabs(hz);
    if (a >= 1e9) {
        std::snprintf(buf, sizeof(buf), "%.9f", hz / 1e9);
        return trimZeros(buf, 3) + " GHz";
    }
    if (a >= 1e6) {
        std::snprintf(buf, sizeof(buf), "%.6f", hz / 1e6);
        return trimZeros(buf, 3) + " MHz";
    }
    if (a >= 1e3) {
        std::snprintf(buf, sizeof(buf), "%.3f", hz / 1e3);
        return trimZeros(buf, 0) + " kHz";
    }
    std::snprintf(buf, sizeof(buf), "%.0f Hz", hz);
    return buf;
}

std::string formatSampleRate(double sps) {
    char buf[64];
    if (sps >= 1e6) std::snprintf(buf, sizeof(buf), "%.6f", sps / 1e6);
    else std::snprintf(buf, sizeof(buf), "%.3f", sps / 1e3);
    return trimZeros(buf, 0) + (sps >= 1e6 ? " MS/s" : " kS/s");
}

std::string formatBandwidth(double hz) {
    char buf[64];
    if (hz >= 1e6) {
        std::snprintf(buf, sizeof(buf), "%.3f", hz / 1e6);
        return trimZeros(buf, 0) + " MHz";
    }
    if (hz >= 1e3) {
        std::snprintf(buf, sizeof(buf), "%.3f", hz / 1e3);
        return trimZeros(buf, 0) + " kHz";
    }
    std::snprintf(buf, sizeof(buf), "%.0f Hz", hz);
    return buf;
}

bool parseFrequency(const std::string& text, double* hz) {
    std::string s;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '_' && c != ',') s.push_back(c);
    if (s.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    std::string unit(end);
    for (auto& c : unit) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (unit.size() >= 2 && unit.compare(unit.size() - 2, 2, "hz") == 0) unit.resize(unit.size() - 2);
    if (unit.empty()) {
    } else if (unit == "k") {
        v *= 1e3;
    } else if (unit == "m") {
        v *= 1e6;
    } else if (unit == "g") {
        v *= 1e9;
    } else {
        return false;
    }
    if (!std::isfinite(v) || v < 0.0) return false;
    *hz = v;
    return true;
}

bool parseNumber(const std::string& text, double* value) {
    std::string s;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '_' && c != ',') s.push_back(c);
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    switch (*end) {
        case 'k':
        case 'K': v *= 1e3; break;
        case 'M': v *= 1e6; break;
        case 'g':
        case 'G': v *= 1e9; break;
        case 'm':  // "1.2m" / "1.2mhz": mega, as in parseFrequency()
            if (end[1] == '\0' || std::tolower(static_cast<unsigned char>(end[1])) == 'h') v *= 1e6;
            break;
        default: break;  // other units ("dB", "Hz", "%") are ignored
    }
    if (!std::isfinite(v)) return false;
    *value = v;
    return true;
}

}  // namespace SharkSDR::ui
