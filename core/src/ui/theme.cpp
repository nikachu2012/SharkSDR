#include "ui/theme.h"

namespace SharkSDR::ui {

namespace {
Fonts g_fonts;
}

const Fonts& fonts() { return g_fonts; }
void setFonts(const Fonts& f) { g_fonts = f; }

void applyTheme(Theme theme, float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    const float fontSizeBase = style.FontSizeBase;
    const float fontScaleMain = style.FontScaleMain;
    style = ImGuiStyle();
    style.FontSizeBase = fontSizeBase;
    style.FontScaleMain = fontScaleMain;

    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(6, 5);
    style.ItemInnerSpacing = ImVec2(5, 4);
    style.ScrollbarSize = 12;
    style.GrabMinSize = 10;
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.WindowRounding = 6;
    style.ChildRounding = 4;
    style.FrameRounding = 4;
    style.PopupRounding = 4;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 3;
    style.TabRounding = 4;
    style.SeparatorTextBorderSize = 1;

    ImVec4* c = style.Colors;
    if (theme == Theme::Light) {
        ImGui::StyleColorsLight(&style);
        c[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.95f, 0.96f, 1.00f);
        c[ImGuiCol_ChildBg] = ImVec4(0.97f, 0.97f, 0.98f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.25f, 0.61f, 1.00f, 0.25f);
        c[ImGuiCol_Button] = ImVec4(0.84f, 0.86f, 0.89f, 1.00f);
        c[ImGuiCol_CheckMark] = ImVec4(0.12f, 0.47f, 0.95f, 1.00f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.12f, 0.47f, 0.95f, 1.00f);
    } else {
        const ImVec4 bg0(0.075f, 0.082f, 0.094f, 1.00f);
        const ImVec4 bg1(0.105f, 0.114f, 0.129f, 1.00f);
        const ImVec4 bg2(0.145f, 0.157f, 0.176f, 1.00f);
        const ImVec4 bg3(0.196f, 0.212f, 0.235f, 1.00f);
        const ImVec4 accent(0.25f, 0.61f, 1.00f, 1.00f);
        const ImVec4 accentHover(0.36f, 0.68f, 1.00f, 1.00f);
        c[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
        c[ImGuiCol_WindowBg] = bg1;
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.135f, 0.98f);
        c[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg] = bg2;
        c[ImGuiCol_FrameBgHovered] = bg3;
        c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.26f, 0.29f, 1.00f);
        c[ImGuiCol_TitleBg] = bg0;
        c[ImGuiCol_TitleBgActive] = bg0;
        c[ImGuiCol_TitleBgCollapsed] = bg0;
        c[ImGuiCol_MenuBarBg] = bg0;
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = bg3;
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.32f, 0.36f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.36f, 0.38f, 0.42f, 1.00f);
        c[ImGuiCol_CheckMark] = accent;
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_SliderGrabActive] = accentHover;
        c[ImGuiCol_Button] = bg2;
        c[ImGuiCol_ButtonHovered] = bg3;
        c[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.61f, 1.00f, 0.55f);
        c[ImGuiCol_Header] = ImVec4(0.25f, 0.61f, 1.00f, 0.22f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.61f, 1.00f, 0.32f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.61f, 1.00f, 0.45f);
        c[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
        c[ImGuiCol_SeparatorHovered] = accent;
        c[ImGuiCol_SeparatorActive] = accentHover;
        c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered] = accent;
        c[ImGuiCol_ResizeGripActive] = accentHover;
        c[ImGuiCol_Tab] = bg2;
        c[ImGuiCol_TabHovered] = bg3;
        c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.36f, 0.58f, 1.00f);
        c[ImGuiCol_PlotHistogram] = accent;
        c[ImGuiCol_TableHeaderBg] = bg2;
        c[ImGuiCol_TableBorderStrong] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
        c[ImGuiCol_TableBorderLight] = ImVec4(1.0f, 1.0f, 1.0f, 0.05f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
        c[ImGuiCol_TextSelectedBg] = ImVec4(0.25f, 0.61f, 1.00f, 0.35f);
        c[ImGuiCol_NavCursor] = accent;
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    }

    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

}  // namespace SharkSDR::ui
