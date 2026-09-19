#pragma once

#include "implot.h"
#include "photon_ui/plot_color.hpp"
#include "photon_ui/plot_time.hpp"
#include "photon_ui/theme.hpp"
#include "photon_ui/view_types.hpp"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace photon::ui {

inline bool parseHexColor(std::string_view text, ImVec4* out) {
    if (out == nullptr) {
        return false;
    }
    float rgba[4]{};
    if (!parseHexColor(text, rgba)) {
        return false;
    }
    out->x = rgba[0];
    out->y = rgba[1];
    out->z = rgba[2];
    out->w = rgba[3];
    return true;
}

inline std::string formatHexColor(const ImVec4& c) {
    return formatHexColor(c.x, c.y, c.z, c.w);
}

inline ImVec4 seriesColor(const ViewSignal& src, std::size_t index) {
    ImVec4 parsed{};
    if (parseHexColor(src.color, &parsed)) {
        return parsed;
    }
    return theme::plotSeries(index);
}

// Distinct swatches so series stay separable on void-black (theme series are near-white).
inline constexpr ImVec4 kPlotColorSwatches[] = {
    {1.00f, 1.00f, 1.00f, 1.0f}, {0.95f, 0.22f, 0.25f, 1.0f}, {1.00f, 0.62f, 0.18f, 1.0f},
    {0.98f, 0.84f, 0.22f, 1.0f}, {0.36f, 0.86f, 0.48f, 1.0f}, {0.22f, 0.82f, 0.86f, 1.0f},
    {0.32f, 0.56f, 0.98f, 1.0f}, {0.78f, 0.42f, 0.98f, 1.0f}, {0.98f, 0.42f, 0.70f, 1.0f},
};

// ImPlot left-click on a legend entry toggles hide. We own hide via src.hidden
// and use the click to open a color picker instead.
inline void keepPlotSeriesShown() { ImPlot::HideNextItem(false, ImPlotCond_Always); }

inline bool plotLegendEntryHot(const char* label) {
    return label != nullptr && label[0] != '\0' && ImPlot::IsLegendEntryHovered(label);
}

// Click (left or right) a legend entry to pin a series color. Returns true if
// colorHex / hidden changed. Call after PlotLine/PlotScatter with the same label.
inline bool drawPlotLegendColorPopup(const char* label, std::string& colorHex, std::size_t themeIndex,
                                     bool* hidden = nullptr) {
    if (label == nullptr || label[0] == '\0') {
        return false;
    }
    if (ImPlot::IsLegendEntryHovered(label)) {
        ImGui::SetTooltip("Click to change color");
    }
    ImGuiMouseButton btn = ImGuiMouseButton_Left;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        btn = ImGuiMouseButton_Right;
    }
    if (!ImPlot::BeginLegendPopup(label, btn)) {
        return false;
    }
    bool changed = false;
    ImVec4 col = theme::plotSeries(themeIndex);
    parseHexColor(colorHex, &col);
    float rgba[4] = {col.x, col.y, col.z, col.w};
    ImGui::TextUnformatted(label);
    if (ImGui::ColorEdit4("##legend-color", rgba,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                              ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_PickerHueWheel)) {
        colorHex = formatHexColor(ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
        changed = true;
    }
    const int nSwatch = static_cast<int>(sizeof(kPlotColorSwatches) / sizeof(kPlotColorSwatches[0]));
    for (int i = 0; i < nSwatch; ++i) {
        ImGui::PushID(i);
        if (i > 0) {
            ImGui::SameLine(0.0f, 3.0f);
        }
        if (ImGui::ColorButton("##sw", kPlotColorSwatches[i],
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(16.0f, 16.0f))) {
            colorHex = formatHexColor(kPlotColorSwatches[i]);
            changed = true;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Theme")) {
        colorHex.clear();
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Use the theme series color");
    }
    if (hidden != nullptr) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Hide", hidden)) {
            changed = true;
        }
    }
    ImPlot::EndLegendPopup();
    return changed;
}

// Time under the mouse from plot layout (ImGui coords), not ImPlot pixel
// conversion — that path can map a click on later data to t≈0 / the past.
inline double plotTimeAtMouse(double axisLo, double axisHi) {
    const ImVec2 pos = ImPlot::GetPlotPos();
    const ImVec2 sz = ImPlot::GetPlotSize();
    if (sz.x > 1.0f && axisHi > axisLo) {
        const double u = static_cast<double>((ImGui::GetIO().MousePos.x - pos.x) / sz.x);
        return axisLo + u * (axisHi - axisLo);
    }
    const ImPlotPoint p = ImPlot::GetPlotMousePos();
    return std::isfinite(p.x) ? p.x : axisHi;
}

inline double clampToPlotAxis(double t, double axisLo, double axisHi) {
    if (!(axisHi > axisLo)) {
        return clampPlotTimeMs(t);
    }
    return std::min(axisHi, std::max(axisLo, t));
}

// Two-finger left/right (MouseWheelH, or shift+wheel) → time delta. Right = later.
inline double plotWheelScrubDeltaMs(double spanMs) {
    const ImGuiIO& io = ImGui::GetIO();
    float h = io.MouseWheelH;
    if (io.KeyShift && std::fabs(h) < 0.01f) {
        h = io.MouseWheel;
    }
    if (std::fabs(h) < 0.01f) {
        return 0.0;
    }
    if (!(spanMs > 1.0)) {
        spanMs = 1000.0;
    }
    return static_cast<double>(h) * spanMs * 0.12;
}

inline bool plotWheelIsZoom() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift) {
        return false;
    }
    return std::fabs(io.MouseWheel) > 0.0f && std::fabs(io.MouseWheelH) < 0.01f;
}

// Wheel / RANGE zoom anchors on the playhead, not the mouse.
inline double plotZoomAnchorMs(double cursorMs, double axisLo, double axisHi) {
    if (std::isfinite(cursorMs) && cursorMs > 0.0) {
        return cursorMs;
    }
    if (std::isfinite(axisHi) && std::isfinite(axisLo) && axisHi > axisLo) {
        return axisHi;
    }
    if (std::isfinite(axisHi) && axisHi > 0.0) {
        return axisHi;
    }
    return clampPlotTimeMs(cursorMs);
}

inline double plotPlayheadMs(double cursorMs, double axisLo, double axisHi) {
    if (std::isfinite(cursorMs) && cursorMs > 0.0) {
        return cursorMs;
    }
    if (std::isfinite(axisHi) && axisHi > 0.0) {
        return axisHi;
    }
    if (std::isfinite(axisLo) && axisLo > 0.0) {
        return axisLo;
    }
    return 0.0;
}

inline ImPlotAxisFlags plotTimeAxisFlags() {
    return ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoHighlight |
           ImPlotAxisFlags_NoSideSwitch | ImPlotAxisFlags_NoMenus;
}

inline ImPlotAxisFlags plotValueAxisFlags(bool autoY, bool noLabel = true) {
    ImPlotAxisFlags flags = ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoSideSwitch |
                            ImPlotAxisFlags_NoMenus;
    if (noLabel) {
        flags |= ImPlotAxisFlags_NoLabel;
    }
    if (autoY) {
        flags |= ImPlotAxisFlags_Lock;
    }
    return flags;
}

struct PlotCursorHit {
    char label[96]{};
    ImVec4 color{};
    double y = 0.0;
    bool ok = false;
    ImAxis yAxis = ImAxis_Y1;
};

inline void setPlotCursorLabel(PlotCursorHit& hit, const char* name, double y) {
    if (name != nullptr && name[0] != '\0') {
        std::snprintf(hit.label, sizeof(hit.label), "%s  %.4g", name, y);
    } else {
        std::snprintf(hit.label, sizeof(hit.label), "%.4g", y);
    }
}

// Marker + value chip at tMs on each series. drawLine is for hover-only cursors;
// the playhead DragLineX already draws the giant vertical line.
inline void drawPlotCursorReadout(double tMs, const PlotCursorHit* hits, int nHits, bool drawLine = true) {
    if (!std::isfinite(tMs) || hits == nullptr || nHits <= 0) {
        return;
    }
    ImPlot::PushPlotClipRect();
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImPlotRect lim = ImPlot::GetPlotLimits();
    if (drawLine) {
        const ImVec2 top = ImPlot::PlotToPixels(tMs, lim.Y.Max);
        const ImVec2 bot = ImPlot::PlotToPixels(tMs, lim.Y.Min);
        if (std::isfinite(top.x) && std::isfinite(bot.x)) {
            dl->AddLine(top, bot, IM_COL32(240, 240, 240, 150), 1.0f);
        }
    }

    const ImVec2 plotMin = ImPlot::GetPlotPos();
    const ImVec2 plotSz = ImPlot::GetPlotSize();
    const float plotRight = plotMin.x + plotSz.x;
    const float fontPx = 13.0f;
    int stacked = 0;
    for (int i = 0; i < nHits; ++i) {
        const PlotCursorHit& hit = hits[i];
        if (!hit.ok || !std::isfinite(hit.y)) {
            continue;
        }
        if (hit.yAxis == ImAxis_Y2) {
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
        } else {
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        }
        const ImVec2 pt = ImPlot::PlotToPixels(tMs, hit.y);
        if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
            continue;
        }
        const ImU32 col = ImGui::GetColorU32(hit.color);
        dl->AddCircleFilled(pt, 4.0f, col);
        dl->AddCircle(pt, 4.0f, IM_COL32(0, 0, 0, 180), 0, 1.25f);
        const char* line = hit.label[0] != '\0' ? hit.label : "";
        const ImVec2 textSz = ImGui::GetFont()->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, line);
        const ImVec2 pad(5.0f, 2.0f);
        float x = pt.x + 8.0f;
        if (x + textSz.x + pad.x * 2.0f > plotRight) {
            x = pt.x - 8.0f - textSz.x - pad.x * 2.0f;
        }
        float y = pt.y - textSz.y * 0.5f + static_cast<float>(stacked) * (textSz.y + 5.0f);
        y = std::max(plotMin.y + 2.0f, std::min(y, plotMin.y + plotSz.y - textSz.y - pad.y * 2.0f - 2.0f));
        const ImVec2 boxMin(x, y);
        const ImVec2 boxMax(x + textSz.x + pad.x * 2.0f, y + textSz.y + pad.y * 2.0f);
        dl->AddRectFilled(boxMin, boxMax, IM_COL32(12, 12, 12, 220), 3.0f);
        dl->AddRect(boxMin, boxMax, col, 3.0f);
        dl->AddText(nullptr, fontPx, ImVec2(boxMin.x + pad.x, boxMin.y + pad.y), IM_COL32(245, 245, 245, 255), line);
        ++stacked;
    }
    ImPlot::PopPlotClipRect();
}

}  // namespace photon::ui
