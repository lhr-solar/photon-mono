#include "photon_ui/theme.hpp"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace photon::ui::theme {
namespace {

ImFont* g_uiFont = nullptr;
ImFont* g_iconFont = nullptr;

struct State {
    Mode mode = Mode::Dark;
    ColorScheme colors = kDark;
    ImVec4 seed = kDefaultSeed;
};

State g_state{};

float mixf(float lo, float hi, float t) { return lo + (hi - lo) * t; }

float randomUnit() {
    static std::mt19937 rng{std::random_device{}()};
    static std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    return dist(rng);
}

std::array<float, 3> oklabToLinearSrgb(float L, float a, float b) {
    const float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
    const float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
    const float s_ = L - 0.0894841775f * a - 1.2914855480f * b;

    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;

    return {
        +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s,
    };
}

std::array<float, 3> oklchToOklab(float L, float c, float h) {
    return {L, c * std::cos(h), c * std::sin(h)};
}

ImVec4 tintTowardWhite(ImVec4 color, float amount) {
    return ImVec4(color.x + (1.0f - color.x) * amount, color.y + (1.0f - color.y) * amount,
                  color.z + (1.0f - color.z) * amount, 1.0f);
}

float seedToHueBase(ImVec4 seed) {
    const float r = std::clamp(seed.x, 0.0f, 1.0f);
    const float g = std::clamp(seed.y, 0.0f, 1.0f);
    const float b = std::clamp(seed.z, 0.0f, 1.0f);
    const float maxv = std::max({r, g, b});
    const float minv = std::min({r, g, b});
    const float delta = maxv - minv;
    if (delta <= 0.0f) {
        return 0.0f;
    }

    float hue = 0.0f;
    if (maxv == r) {
        hue = std::fmod((g - b) / delta, 6.0f);
    } else if (maxv == g) {
        hue = (b - r) / delta + 2.0f;
    } else {
        hue = (r - g) / delta + 4.0f;
    }

    hue /= 6.0f;
    if (hue < 0.0f) {
        hue += 1.0f;
    }
    return hue;
}

std::array<float, 3> seedToHsv(ImVec4 seed) {
    const float r = std::clamp(seed.x, 0.0f, 1.0f);
    const float g = std::clamp(seed.y, 0.0f, 1.0f);
    const float b = std::clamp(seed.z, 0.0f, 1.0f);
    const float maxv = std::max({r, g, b});
    const float minv = std::min({r, g, b});
    const float delta = maxv - minv;
    const float saturation = maxv <= 0.0f ? 0.0f : delta / maxv;
    return {seedToHueBase(seed), saturation, maxv};
}

void applyImGui(const ColorScheme& scheme) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = kFontSize;
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.55f;
    style.TabRounding = kFrameRounding;
    style.WindowRounding = kPopupRounding;
    style.ChildRounding = kPanelRounding;
    style.PopupRounding = kPopupRounding;
    style.GrabRounding = kFrameRounding;
    style.ScrollbarRounding = kScrollbarRounding;
    style.FrameRounding = kFrameRounding;
    style.FrameBorderSize = 0.0f;
    style.WindowBorderSize = kBorderSize;
    style.ChildBorderSize = kBorderSize;
    style.PopupBorderSize = kBorderSize;
    style.TabBorderSize = 0.0f;
    style.DockingSeparatorSize = 1.0f;
    style.WindowPadding = kWindowPadding;
    style.FramePadding = kFramePadding;
    style.CellPadding = kCellPadding;
    style.ItemSpacing = kItemSpacing;
    style.ItemInnerSpacing = ImVec2{8.0f, 6.0f};
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = kScrollbarSize;
    style.GrabMinSize = 10.0f;
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;
    style.CurveTessellationTol = 0.10f;
    style.CircleTessellationMaxError = 0.05f;
    ImGui::GetIO().Fonts->Flags &= ~ImFontAtlasFlags_NoBakedLines;

    const ImVec4 color0 = scheme.color0;
    const ImVec4 color1 = scheme.color1;
    const ImVec4 color2 = scheme.color2;
    const ImVec4 color3 = scheme.color3;
    const ImVec4 color4 = scheme.color4;
    const ImVec4 color5 = scheme.color5;
    const ImVec4 color6 = scheme.color6;
    const ImVec4 color7 = scheme.color7;
    const ImVec4 panel = mixColor(color7, color4, 0.30f);
    const ImVec4 raised = mixColor(color7, color4, 0.62f);
    const ImVec4 active = mixColor(color4, color1, 0.38f);
    const ImVec4 border = mixColor(color4, color0, 0.20f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = color0;
    c[ImGuiCol_TextDisabled] = color3;
    c[ImGuiCol_WindowBg] = color7;
    c[ImGuiCol_ChildBg] = withAlpha(panel, 0.76f);
    c[ImGuiCol_PopupBg] = withAlpha(color7, 0.98f);
    c[ImGuiCol_Border] = withAlpha(border, 0.55f);
    c[ImGuiCol_BorderShadow] = withAlpha(color7, 0.0f);
    c[ImGuiCol_FrameBg] = withAlpha(panel, 0.82f);
    c[ImGuiCol_FrameBgHovered] = withAlpha(raised, 0.88f);
    c[ImGuiCol_FrameBgActive] = withAlpha(raised, 0.96f);
    c[ImGuiCol_TitleBg] = color6;
    c[ImGuiCol_TitleBgActive] = color5;
    c[ImGuiCol_TitleBgCollapsed] = color7;
    c[ImGuiCol_MenuBarBg] = color5;
    c[ImGuiCol_ScrollbarBg] = withAlpha(panel, 0.18f);
    c[ImGuiCol_ScrollbarGrab] = withAlpha(raised, 0.72f);
    c[ImGuiCol_ScrollbarGrabHovered] = withAlpha(mixColor(raised, active, 0.42f), 0.86f);
    c[ImGuiCol_ScrollbarGrabActive] = withAlpha(active, 0.94f);
    c[ImGuiCol_CheckMark] = color1;
    c[ImGuiCol_SliderGrab] = color2;
    c[ImGuiCol_SliderGrabActive] = color1;
    c[ImGuiCol_Button] = withAlpha(raised, 0.80f);
    c[ImGuiCol_ButtonHovered] = withAlpha(active, 0.72f);
    c[ImGuiCol_ButtonActive] = withAlpha(active, 0.88f);
    c[ImGuiCol_Header] = withAlpha(active, 0.42f);
    c[ImGuiCol_HeaderHovered] = withAlpha(active, 0.52f);
    c[ImGuiCol_HeaderActive] = withAlpha(active, 0.68f);
    c[ImGuiCol_Separator] = color3;
    c[ImGuiCol_SeparatorHovered] = color2;
    c[ImGuiCol_SeparatorActive] = color1;
    c[ImGuiCol_ResizeGrip] = color2;
    c[ImGuiCol_ResizeGripHovered] = color1;
    c[ImGuiCol_ResizeGripActive] = color0;
    c[ImGuiCol_Tab] = color7;
    c[ImGuiCol_TabHovered] = color3;
    c[ImGuiCol_TabSelected] = color4;
    c[ImGuiCol_TabSelectedOverline] = color4;
    c[ImGuiCol_TabDimmed] = color7;
    c[ImGuiCol_TabDimmedSelected] = color4;
    c[ImGuiCol_TabDimmedSelectedOverline] = color4;
    c[ImGuiCol_DockingPreview] = color1;
    c[ImGuiCol_DockingEmptyBg] = color1;
    c[ImGuiCol_PlotLines] = color1;
    c[ImGuiCol_PlotLinesHovered] = color2;
    c[ImGuiCol_PlotHistogram] = color1;
    c[ImGuiCol_PlotHistogramHovered] = color2;
    c[ImGuiCol_TableHeaderBg] = withAlpha(raised, 0.74f);
    c[ImGuiCol_TableBorderStrong] = withAlpha(border, 0.42f);
    c[ImGuiCol_TableBorderLight] = withAlpha(border, 0.30f);
    c[ImGuiCol_TableRowBg] = withAlpha(panel, 0.34f);
    c[ImGuiCol_TableRowBgAlt] = withAlpha(raised, 0.28f);
    c[ImGuiCol_TextSelectedBg] = color3;
    c[ImGuiCol_DragDropTarget] = color3;
    c[ImGuiCol_NavCursor] = color1;
    c[ImGuiCol_NavWindowingHighlight] = color1;
    c[ImGuiCol_NavWindowingDimBg] = {color1.x, color1.y, color1.z, 0.2f};
    c[ImGuiCol_ModalWindowDimBg] = {color0.x, color0.y, color0.z, 0.4f};
}

void applyImPlot(const ColorScheme& scheme) {
    ImPlotStyle& plotStyle = ImPlot::GetStyle();
    plotStyle.UseLocalTime = true;
    plotStyle.LineWeight = kPlotLineWeight;
    plotStyle.Colormap = ImPlotColormap_Deep;

    const ImVec4 color0 = scheme.color0;
    const ImVec4 color1 = scheme.color1;
    const ImVec4 color3 = scheme.color3;
    const ImVec4 color4 = scheme.color4;
    const ImVec4 color5 = scheme.color5;

    ImVec4* c = plotStyle.Colors;
    c[ImPlotCol_FrameBg] = color5;
    c[ImPlotCol_PlotBg] = color4;
    c[ImPlotCol_PlotBorder] = color5;
    c[ImPlotCol_LegendBg] = color4;
    c[ImPlotCol_LegendBorder] = color5;
    c[ImPlotCol_LegendText] = color0;
    c[ImPlotCol_TitleText] = color0;
    c[ImPlotCol_InlayText] = color1;
    c[ImPlotCol_AxisText] = color1;
    c[ImPlotCol_AxisGrid] = color1;
    c[ImPlotCol_AxisTick] = color1;
    c[ImPlotCol_AxisBg] = color5;
    c[ImPlotCol_AxisBgHovered] = color4;
    c[ImPlotCol_AxisBgActive] = color3;
    c[ImPlotCol_Selection] = color1;
    c[ImPlotCol_Crosshairs] = color1;
}

}  // namespace

const ColorScheme& scheme() { return g_state.colors; }

ImFont* uiFont() { return g_uiFont; }

ImFont* iconFont() { return g_iconFont; }

bool loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigDpiScaleFonts = true;
    io.FontGlobalScale = 1.0f;

    // Legacy gpu.cpp imguiBackend: Inter 28pt Regular, then TablerIcons 28pt
    // (glyph range 0xEA00-0xFBFF, OversampleH/V=2), then Inter tiers 16/24/32.
    // ImGui 1.92 rasterizes dynamically; FontSizeBase (14) is the UI size.
    // Loading at 28pt matches legacy LegacySize so PushFont(font, 14/11/24)
    // stays sharp at devicePixelRatio 1 and 2 via DisplayFramebufferScale.
    ImFontConfig fontConfig;
    fontConfig.FontDataOwnedByAtlas = true;
    g_uiFont = io.Fonts->AddFontFromFileTTF("/fonts/Inter_28pt-Regular.ttf", 28.0f, &fontConfig);
    if (g_uiFont == nullptr) {
        return false;
    }

    static const ImWchar tablerIconRanges[] = {0xEA00, 0xFBFF, 0};
    ImFontConfig tablerFontConfig;
    tablerFontConfig.FontDataOwnedByAtlas = true;
    tablerFontConfig.PixelSnapH = false;
    tablerFontConfig.OversampleH = 2;
    tablerFontConfig.OversampleV = 2;
    g_iconFont = io.Fonts->AddFontFromFileTTF("/fonts/TablerIcons.ttf", 28.0f, &tablerFontConfig,
                                              tablerIconRanges);

    for (float tierSize : {16.0f, 24.0f, 32.0f}) {
        ImFontConfig tierConfig;
        tierConfig.FontDataOwnedByAtlas = true;
        (void)io.Fonts->AddFontFromFileTTF("/fonts/Inter_28pt-Regular.ttf", tierSize, &tierConfig);
    }
    io.FontDefault = g_uiFont;
    return true;
}

Mode mode() { return g_state.mode; }

ColorScheme generate(ImVec4 seed) {
    const auto hsv = seedToHsv(seed);

    const float hueBaseSetting = hsv[0];
    const float hueContrastSetting = 0.8f;
    const float saturationBase = randomUnit();
    const float saturationContrast = randomUnit();
    const float luminanceBase = hsv[2];
    const float luminanceContrast = 0.8f;
    const float fixed = 0.1f;
    const bool saturationConstant = true;
    constexpr int kColorCount = 8;

    constexpr float kPi = 3.14159265358979323846f;
    const float hueBase = hueBaseSetting * 2.0f * kPi;
    const float hueContrast = mixf(0.33f, 1.0f, hueContrastSetting);
    const float chromaBase = mixf(0.01f, 0.1f, saturationBase);
    const float chromaContrast = mixf(0.075f, 0.125f - chromaBase, saturationContrast);
    const float chromaFixed = mixf(0.01f, 0.125f, fixed);
    const float lightnessBase = mixf(0.3f, 0.6f, luminanceBase);
    const float lightnessContrast = mixf(0.3f, 1.0f - lightnessBase, luminanceContrast);

    ColorScheme out{};
    ImVec4* stops[] = {&out.color7, &out.color6, &out.color5, &out.color4,
                       &out.color3, &out.color2, &out.color1, &out.color0};

    for (int i = 0; i < kColorCount; ++i) {
        const float linearIterator = static_cast<float>(i) / static_cast<float>(kColorCount - 1);
        float hueOffset = linearIterator * hueContrast * 2.0f * kPi + (kPi / 4.0f);
        hueOffset *= 0.33f;
        hueOffset += (randomUnit() * 2.0f - 1.0f) * 0.01f;

        const float chroma =
            saturationConstant ? chromaFixed : chromaBase + linearIterator * chromaContrast;
        const float lightness =
            saturationConstant ? lightnessBase + linearIterator * lightnessContrast : mixf(0.6f, 0.9f, fixed);

        const auto lab = oklchToOklab(lightness, chroma, hueBase + hueOffset);
        auto rgb = oklabToLinearSrgb(lab[0], lab[1], lab[2]);
        rgb[0] = std::clamp(rgb[0], 0.0f, 1.0f);
        rgb[1] = std::clamp(rgb[1], 0.0f, 1.0f);
        rgb[2] = std::clamp(rgb[2], 0.0f, 1.0f);
        *stops[i] = ImVec4(rgb[0], rgb[1], rgb[2], 1.0f);
    }

    out.color0 = tintTowardWhite(out.color0, 0.9f);
    return out;
}

void setMode(Mode next, ImVec4 seed) {
    g_state.mode = next;
    g_state.seed = seed;
    if (next == Mode::Light) {
        g_state.colors = kLight;
    } else if (next == Mode::Custom) {
        g_state.colors = generate(seed);
    } else {
        g_state.colors = kDark;
    }
}

void setScheme(const ColorScheme& next) {
    g_state.mode = Mode::Custom;
    g_state.colors = next;
}

void apply() {
    applyImGui(g_state.colors);
    applyImPlot(g_state.colors);
}

ImVec4 plotSeries(std::size_t index) {
    const int n = ImPlot::GetColormapSize();
    const int i = n > 0 ? static_cast<int>(index % static_cast<std::size_t>(n)) : 0;
    return ImPlot::GetColormapColor(i);
}

float plotLineWeight() { return kPlotLineWeight; }

void pushTableStyle() {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, kContentCellPadding);
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, withAlpha(muted(), 0.76f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, withAlpha(border(), 0.56f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, withAlpha(border(), 0.42f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg, withAlpha(card(), 0.46f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, withAlpha(muted(), 0.34f));
}

void popTableStyle() {
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(1);
}

bool beginCard(const char* id, ImVec2 size, ImVec4 bg, ImVec2 padding, CardFit fit) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, withAlpha(border(), 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, cardRounding(fit));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    return ImGui::BeginChild(id, size,
                             ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                             ImGuiWindowFlags_NoScrollbar);
}

void endCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

}  // namespace photon::ui::theme
