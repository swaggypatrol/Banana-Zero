#include "menu_style.h"

namespace
{
MenuTheme g_theme = MenuTheme::Dark;

ImVec4 Rgb(unsigned hex, float alpha = 1.0f)
{
    return ImVec4(float((hex >> 16) & 0xFF) / 255.0f, float((hex >> 8) & 0xFF) / 255.0f, float(hex & 0xFF) / 255.0f,
                  alpha);
}

// The dark theme: ImGui's own dark colours, a little rounding. What the menu looked like first.
void Dark(ImGuiStyle& style)
{
    ImGui::StyleColorsDark(&style);
    style.Colors[ImGuiCol_TitleBgCollapsed] = style.Colors[ImGuiCol_TitleBg]; // opaque: the arrow is drawn over it
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
}

// The paper theme: warm, low-saturation kraft paper with a clay accent, dark warm text. Nothing on it is
// saturated; the accent carries the only hue.
constexpr unsigned kPaper = 0xEAE3D6;      // the window: kraft paper
constexpr unsigned kPaperDeep = 0xDDD5C5;  // frames: a deeper paper
constexpr unsigned kPaperHover = 0xD3CAB8; // hovered frames and buttons
constexpr unsigned kPaperPress = 0xC8BDA9; // pressed
constexpr unsigned kPaperTitle = 0xE1D9CA; // the title bar
constexpr unsigned kLine = 0xC9BFAE;       // borders and separators
constexpr unsigned kInk = 0x4A443D;        // text: warm dark grey, not black
constexpr unsigned kInkFaint = 0x8A8275;   // disabled text, notes
constexpr unsigned kClay = 0xD97757;       // the accent: clay
constexpr unsigned kClayDeep = 0xC0532F;   // the accent pressed, warnings
constexpr unsigned kOchre = 0x96691C;      // attention
constexpr unsigned kSlate = 0x4C6B84;      // information

void Paper(ImGuiStyle& style)
{
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Rgb(kInk);
    c[ImGuiCol_TextDisabled] = Rgb(kInkFaint);
    c[ImGuiCol_WindowBg] = Rgb(kPaper, 0.97f);
    c[ImGuiCol_ChildBg] = Rgb(kPaper, 0.0f);
    c[ImGuiCol_PopupBg] = Rgb(0xEFE9DE, 0.99f);
    c[ImGuiCol_Border] = Rgb(kLine, 0.9f);
    c[ImGuiCol_BorderShadow] = Rgb(0, 0.0f);
    c[ImGuiCol_FrameBg] = Rgb(kPaperDeep);
    c[ImGuiCol_FrameBgHovered] = Rgb(kPaperHover);
    c[ImGuiCol_FrameBgActive] = Rgb(kPaperPress);
    c[ImGuiCol_TitleBg] = Rgb(kPaperTitle);
    c[ImGuiCol_TitleBgActive] = Rgb(kPaperTitle);
    c[ImGuiCol_TitleBgCollapsed] = Rgb(kPaperTitle);
    c[ImGuiCol_MenuBarBg] = Rgb(kPaperTitle);
    c[ImGuiCol_ScrollbarBg] = Rgb(kPaper, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = Rgb(kLine);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(0xB8AE9C);
    c[ImGuiCol_ScrollbarGrabActive] = Rgb(0xA99F8C);
    c[ImGuiCol_CheckMark] = Rgb(kClayDeep);
    c[ImGuiCol_SliderGrab] = Rgb(kClay);
    c[ImGuiCol_SliderGrabActive] = Rgb(kClayDeep);
    c[ImGuiCol_Button] = Rgb(kPaperDeep);
    c[ImGuiCol_ButtonHovered] = Rgb(kPaperHover);
    c[ImGuiCol_ButtonActive] = Rgb(kPaperPress);
    c[ImGuiCol_Header] = Rgb(kPaperDeep);
    c[ImGuiCol_HeaderHovered] = Rgb(kPaperHover);
    c[ImGuiCol_HeaderActive] = Rgb(kPaperPress);
    c[ImGuiCol_Separator] = Rgb(kLine);
    c[ImGuiCol_SeparatorHovered] = Rgb(kClay);
    c[ImGuiCol_SeparatorActive] = Rgb(kClayDeep);
    c[ImGuiCol_ResizeGrip] = Rgb(kLine, 0.5f);
    c[ImGuiCol_ResizeGripHovered] = Rgb(kClay, 0.7f);
    c[ImGuiCol_ResizeGripActive] = Rgb(kClayDeep);
    c[ImGuiCol_Tab] = Rgb(kPaperDeep);
    c[ImGuiCol_TabHovered] = Rgb(kPaperHover);
    c[ImGuiCol_TabSelected] = Rgb(kPaper);
    c[ImGuiCol_TabSelectedOverline] = Rgb(kClay);
    c[ImGuiCol_TabDimmed] = Rgb(kPaperDeep);
    c[ImGuiCol_TabDimmedSelected] = Rgb(kPaper);
    c[ImGuiCol_TabDimmedSelectedOverline] = Rgb(kLine);
    c[ImGuiCol_PlotLines] = Rgb(kSlate);
    c[ImGuiCol_PlotLinesHovered] = Rgb(kClayDeep);
    c[ImGuiCol_PlotHistogram] = Rgb(kClay);
    c[ImGuiCol_PlotHistogramHovered] = Rgb(kClayDeep);
    c[ImGuiCol_TableHeaderBg] = Rgb(kPaperTitle);
    c[ImGuiCol_TableBorderStrong] = Rgb(kLine);
    c[ImGuiCol_TableBorderLight] = Rgb(kLine, 0.5f);
    c[ImGuiCol_TableRowBg] = Rgb(0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = Rgb(kInk, 0.03f);
    c[ImGuiCol_TextLink] = Rgb(kClayDeep);
    c[ImGuiCol_TextSelectedBg] = Rgb(kClay, 0.3f);
    c[ImGuiCol_DragDropTarget] = Rgb(kClayDeep);
    c[ImGuiCol_NavCursor] = Rgb(kClay, 0.8f);
    c[ImGuiCol_NavWindowingHighlight] = Rgb(kInk, 0.5f);
    c[ImGuiCol_NavWindowingDimBg] = Rgb(kInk, 0.15f);
    c[ImGuiCol_ModalWindowDimBg] = Rgb(0x2A2521, 0.3f);

    style.WindowPadding = ImVec2(18.0f, 14.0f);
    style.FramePadding = ImVec2(10.0f, 4.0f);
    style.ItemSpacing = ImVec2(10.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 14.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowRounding = 10.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextPadding = ImVec2(18.0f, 4.0f);
}
} // namespace

void MenuStyleApply(MenuTheme theme, float scale)
{
    g_theme = theme;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    if (theme == MenuTheme::Paper)
        Paper(style);
    else
        Dark(style);
    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
}

ImVec4 MenuToneColour(MenuTone tone)
{
    if (g_theme == MenuTheme::Paper)
    {
        switch (tone)
        {
        case MenuTone::Warn:
            return Rgb(kClayDeep);
        case MenuTone::Attention:
            return Rgb(kOchre);
        case MenuTone::Info:
            return Rgb(kSlate);
        case MenuTone::Accent:
            return Rgb(kClay);
        }
    }
    switch (tone)
    {
    case MenuTone::Warn:
        return ImVec4(1.0f, 0.5f, 0.4f, 1.0f);
    case MenuTone::Attention:
        return ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
    case MenuTone::Info:
        return ImVec4(0.5f, 0.8f, 1.0f, 1.0f);
    case MenuTone::Accent:
        return ImVec4(1.0f, 0.78f, 0.31f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

ImU32 MenuToneU32(MenuTone tone, float alpha)
{
    ImVec4 c = MenuToneColour(tone);
    c.w *= alpha;
    return ImGui::GetColorU32(c);
}
