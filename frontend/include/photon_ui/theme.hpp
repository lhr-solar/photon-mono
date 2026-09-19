#pragma once

#include "imgui.h"

#include <cstddef>

namespace photon::ui::theme {

// 8-stop Photon palette (legacy GuiSettings::ColorScheme / config.ini [Photon][Theme]).
struct ColorScheme {
    ImVec4 color0{};
    ImVec4 color1{};
    ImVec4 color2{};
    ImVec4 color3{};
    ImVec4 color4{};
    ImVec4 color5{};
    ImVec4 color6{};
    ImVec4 color7{};
};

enum class Mode : int { Dark = 0, Light = 1, Custom = 2, Falcon = 3 };

// Void — Grok / xAI: true black, white type, almost no chroma.
inline constexpr ColorScheme kDark = {
    .color0{0.96f, 0.96f, 0.97f, 1.00f},
    .color1{0.88f, 0.88f, 0.90f, 1.00f},
    .color2{0.62f, 0.63f, 0.66f, 1.00f},
    .color3{0.42f, 0.43f, 0.45f, 1.00f},
    .color4{0.12f, 0.12f, 0.13f, 1.00f},
    .color5{0.07f, 0.07f, 0.08f, 1.00f},
    .color6{0.02f, 0.02f, 0.02f, 1.00f},
    .color7{0.00f, 0.00f, 0.00f, 1.00f},
};

// Falcon — SpaceX mission control: cool graphite, ice silver, mission red chrome.
// Not Void: lifted pad black (not #000), visible graphite cards, red on chrome.
inline constexpr ColorScheme kFalcon = {
    .color0{0.98f, 0.98f, 0.99f, 1.00f},
    .color1{0.78f, 0.80f, 0.84f, 1.00f},
    .color2{0.91f, 0.13f, 0.16f, 1.00f},
    .color3{0.50f, 0.52f, 0.56f, 1.00f},
    .color4{0.18f, 0.19f, 0.22f, 1.00f},
    .color5{0.11f, 0.12f, 0.14f, 1.00f},
    .color6{0.07f, 0.08f, 0.10f, 1.00f},
    .color7{0.03f, 0.04f, 0.05f, 1.00f},
};

inline constexpr ColorScheme kLight = {
    .color0{0.08f, 0.08f, 0.09f, 1.00f},
    .color1{0.18f, 0.18f, 0.20f, 1.00f},
    .color2{0.42f, 0.43f, 0.46f, 1.00f},
    .color3{0.70f, 0.70f, 0.72f, 1.00f},
    .color4{0.90f, 0.90f, 0.91f, 1.00f},
    .color5{0.95f, 0.95f, 0.96f, 1.00f},
    .color6{0.98f, 0.98f, 0.98f, 1.00f},
    .color7{1.00f, 1.00f, 1.00f, 1.00f},
};

inline constexpr ImVec4 kDefaultSeed{0.00f, 0.50f, 0.50f, 1.00f};
inline constexpr float kFontSize = 14.0f;
inline constexpr float kSmallFontSize = 11.0f;
inline constexpr float kLargeFontSize = 24.0f;
inline constexpr float kVitalTileHeight = 72.0f;
inline constexpr float kFrameRounding = 4.0f;
inline constexpr float kPanelRounding = 4.0f;
inline constexpr float kPopupRounding = 6.0f;
inline constexpr float kScrollbarRounding = 999.0f;
inline constexpr float kScrollbarSize = 10.0f;
inline constexpr float kBorderSize = 1.0f;
inline constexpr float kPlotLineWeight = 1.5f;
inline constexpr float kPlotFontSize = 18.0f;
inline constexpr ImVec2 kWindowPadding{14.0f, 12.0f};
inline constexpr ImVec2 kFramePadding{10.0f, 7.0f};
inline constexpr ImVec2 kCellPadding{12.0f, 7.0f};
inline constexpr ImVec2 kItemSpacing{10.0f, 8.0f};
inline constexpr float kGridGap = 10.0f;
inline constexpr float kTitlebarH = 44.0f;
inline constexpr ImVec2 kTitlebarPadding{14.0f, 8.0f};
inline constexpr float kProtectionRowH = 96.0f;
inline constexpr float kHudInset = 16.0f;
inline constexpr ImVec2 kContentWindowPadding{14.0f, 12.0f};
inline constexpr ImVec2 kContentFramePadding{12.0f, 9.0f};
inline constexpr ImVec2 kContentCellPadding{14.0f, 8.0f};
inline constexpr ImVec2 kStripPadding{10.0f, 8.0f};
inline constexpr ImVec2 kTilePadding{10.0f, 8.0f};
inline constexpr float kChipRounding = 3.0f;
inline constexpr float kTinyTileRounding = 2.0f;

// Island: inset card, 8 px. Band: full-width strip in a screen. Flush: window/shell edge.
enum class CardFit : int { Island = 0, Band = 1, Flush = 2 };

inline float cardRounding(CardFit fit) { return fit == CardFit::Island ? kPanelRounding : 0.0f; }

inline ImVec4 withAlpha(ImVec4 c, float a) { return {c.x, c.y, c.z, a}; }

inline ImVec4 mixColor(ImVec4 a, ImVec4 b, float t) {
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t};
}

inline ImU32 u32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

const ColorScheme& scheme();
Mode mode();
const char* modeLabel();

// Seed-based OKLCH generator from legacy GuiSettings::genColors. Custom mode only.
ColorScheme generate(ImVec4 seed);

void setMode(Mode mode, ImVec4 seed = kDefaultSeed);
void setScheme(const ColorScheme& scheme);
void loadPersisted();
void apply();
bool loadFonts();
void drawSwitcher(ImVec2 btnSize);
ImFont* uiFont();
ImFont* iconFont();

// Derived slots — same mixes as GuiSettings::setStyle / PhotonUi::palette().
inline ImVec4 foreground() { return scheme().color0; }
inline ImVec4 primary() { return scheme().color1; }
inline ImVec4 accent() { return scheme().color2; }
inline ImVec4 titlebar() { return scheme().color6; }
inline ImVec4 background() { return scheme().color7; }
inline ImVec4 card() { return mixColor(scheme().color7, scheme().color4, 0.16f); }
inline ImVec4 muted() { return mixColor(scheme().color7, scheme().color4, 0.42f); }
inline ImVec4 active() { return mixColor(scheme().color4, scheme().color1, 0.22f); }
inline ImVec4 border() { return mixColor(scheme().color4, scheme().color0, 0.14f); }
inline ImVec4 mutedFg() { return mixColor(scheme().color0, scheme().color7, 0.42f); }

// Selected chrome: Void stays white; Falcon uses mission red.
inline ImVec4 chrome() { return mode() == Mode::Falcon ? accent() : foreground(); }

// Semantic status — desaturated so they sit on void black without neon.
inline ImVec4 success() { return {0.42f, 0.78f, 0.52f, 1.0f}; }
inline ImVec4 warning() { return {0.92f, 0.72f, 0.38f, 1.0f}; }
inline ImVec4 danger() { return {0.86f, 0.22f, 0.24f, 1.0f}; }

inline float luminance(ImVec4 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

inline ImVec4 contrastingText(ImVec4 fill) {
    return luminance(fill) > 0.45f ? scheme().color7 : scheme().color0;
}

ImVec4 plotSeries(std::size_t index);
float plotLineWeight();

void pushTableStyle();
void popTableStyle();

void pushSmallFont();
void pushLargeFont();
void pushPlotFont();
void popSmallFont();
void popLargeFont();
void popPlotFont();

bool beginCard(const char* id, ImVec2 size, ImVec4 bg, ImVec2 padding, CardFit fit);
inline bool beginCard(const char* id, ImVec2 size, ImVec4 bg, ImVec2 padding = kWindowPadding) {
    return beginCard(id, size, bg, padding, CardFit::Island);
}
inline bool beginCard(const char* id, ImVec2 size, ImVec4 bg, CardFit fit) {
    return beginCard(id, size, bg, kWindowPadding, fit);
}
void endCard();

}  // namespace photon::ui::theme
