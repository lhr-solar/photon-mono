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
#include <limits>
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

// Distinct swatches so series stay separable on void-black (theme series are near-white).
inline constexpr ImVec4 kPlotColorSwatches[] = {
    {1.00f, 1.00f, 1.00f, 1.0f}, {0.95f, 0.22f, 0.25f, 1.0f}, {1.00f, 0.62f, 0.18f, 1.0f},
    {0.98f, 0.84f, 0.22f, 1.0f}, {0.36f, 0.86f, 0.48f, 1.0f}, {0.22f, 0.82f, 0.86f, 1.0f},
    {0.32f, 0.56f, 0.98f, 1.0f}, {0.78f, 0.42f, 0.98f, 1.0f}, {0.98f, 0.42f, 0.70f, 1.0f},
};

inline ImVec4 defaultSeriesColor(std::size_t index) {
    const int n = static_cast<int>(sizeof(kPlotColorSwatches) / sizeof(kPlotColorSwatches[0]));
    return kPlotColorSwatches[index % static_cast<std::size_t>(n)];
}

inline ImVec4 seriesColor(const ViewSignal& src, std::size_t index) {
    ImVec4 parsed{};
    if (parseHexColor(src.color, &parsed)) {
        return parsed;
    }
    return defaultSeriesColor(index);
}

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
    ImGuiMouseButton btn = ImGuiMouseButton_Left;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        btn = ImGuiMouseButton_Right;
    }
    if (!ImPlot::BeginLegendPopup(label, btn)) {
        return false;
    }
    bool changed = false;
    ImVec4 col = defaultSeriesColor(themeIndex);
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
    if (ImGui::SmallButton("Default")) {
        colorHex.clear();
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Use the default series color");
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
    if (io.KeyShift || io.KeyAlt) {
        return false;
    }
    return std::fabs(io.MouseWheel) > 0.0f && std::fabs(io.MouseWheelH) < 0.01f;
}

// Alt+wheel zooms the value axis. Shift stays the time scrub.
inline bool plotWheelIsValueZoom() {
    const ImGuiIO& io = ImGui::GetIO();
    return io.KeyAlt && !io.KeyShift && std::fabs(io.MouseWheel) > 0.0f && std::fabs(io.MouseWheelH) < 0.01f;
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

// Marker + value for the series whose line is under the mouse. mouseY is a
// screen pixel; a non-finite value lists every hit.
// Top-left legend footprint, including ImPlot's outer pad.
inline void reserveNorthwestLegend(bool enabled, const std::vector<std::string>& names, float& w, float& h) {
    w = 0.0f;
    h = 0.0f;
    if (!enabled || names.empty()) {
        return;
    }
    theme::pushSmallFont();
    float maxLabel = 0.0f;
    int n = 0;
    for (const std::string& name : names) {
        if (name.empty()) {
            continue;
        }
        maxLabel = std::max(maxLabel, ImGui::CalcTextSize(name.c_str()).x);
        ++n;
    }
    const float line = ImGui::GetTextLineHeight();
    theme::popSmallFont();
    if (n <= 0) {
        return;
    }
    constexpr float kInner = 5.0f;
    constexpr float kPad = 10.0f;
    const float innerW = kInner * 2.0f + line + maxLabel;
    const float innerH = kInner * 2.0f + static_cast<float>(n) * line;
    w = kPad + innerW + 8.0f;
    h = kPad + innerH + 8.0f;
}

inline bool plotBoxesOverlap(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    return aw > 0.0f && ah > 0.0f && bw > 0.0f && bh > 0.0f && ax < bx + bw && ax + aw > bx && ay < by + bh &&
           ay + ah > by;
}

// legendW/legendH reserve the top-left legend box, in pixels from the plot origin.
inline void drawPlotCursorReadout(double tMs, const PlotCursorHit* hits, int nHits,
                                  float mouseY = std::numeric_limits<float>::quiet_NaN(), float pickPx = 10.0f,
                                  float legendW = 0.0f, float legendH = 0.0f) {
    if (!std::isfinite(tMs) || hits == nullptr || nHits <= 0) {
        return;
    }
    ImPlot::PushPlotClipRect();
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImPlotRect lim = ImPlot::GetPlotLimits();

    const ImVec2 plotMin = ImPlot::GetPlotPos();
    const ImVec2 plotSz = ImPlot::GetPlotSize();
    const float plotRight = plotMin.x + plotSz.x;
    const float fontPx = 13.0f;
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
        if (std::isfinite(mouseY) && std::fabs(pt.y - mouseY) > pickPx) {
            continue;
        }
        const ImU32 col = ImGui::GetColorU32(hit.color);
        dl->AddCircleFilled(pt, 3.5f, col);
        dl->AddCircle(pt, 3.5f, IM_COL32(0, 0, 0, 180), 0, 1.0f);
    }

    float maxText = 0.0f;
    int rows = 0;
    for (int i = 0; i < nHits; ++i) {
        if (!hits[i].ok || hits[i].label[0] == '\0') {
            continue;
        }
        if (hits[i].yAxis == ImAxis_Y2) {
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
        } else {
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
        }
        const ImVec2 pt = ImPlot::PlotToPixels(tMs, hits[i].y);
        if (!std::isfinite(pt.y) || (std::isfinite(mouseY) && std::fabs(pt.y - mouseY) > pickPx)) {
            continue;
        }
        const ImVec2 textSz = ImGui::GetFont()->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, hits[i].label);
        maxText = std::max(maxText, textSz.x);
        ++rows;
    }
    if (rows > 0 && maxText > 0.0f) {
        const float rowH = fontPx + 3.0f;
        const float boxW = std::min(plotSz.x - 12.0f, maxText + 22.0f);
        const float boxH = std::min(plotSz.y - 8.0f, static_cast<float>(rows) * rowH + 8.0f);
        const ImVec2 cursorPx = ImPlot::PlotToPixels(tMs, lim.Y.Max);
        const float xLeft = plotMin.x + 6.0f;
        const float xRight = plotRight - boxW - 6.0f;
        const bool cursorOnLeft = std::isfinite(cursorPx.x) && cursorPx.x < plotMin.x + plotSz.x * 0.5f;
        float x = cursorOnLeft ? xRight : xLeft;
        float y = plotMin.y + 6.0f;
        const float legX = plotMin.x;
        const float legY = plotMin.y;
        if (plotBoxesOverlap(x, y, boxW, boxH, legX, legY, legendW, legendH)) {
            const float other = (x == xLeft) ? xRight : xLeft;
            if (!plotBoxesOverlap(other, y, boxW, boxH, legX, legY, legendW, legendH)) {
                x = other;
            } else {
                y = plotMin.y + legendH + 4.0f;
                x = xLeft;
            }
        }
        if (std::isfinite(cursorPx.x) && cursorPx.x > x - 4.0f && cursorPx.x < x + boxW + 4.0f) {
            const float slid = cursorPx.x - boxW - 10.0f;
            const float minX = plotMin.x + std::max(6.0f, legendW + 6.0f);
            if (slid >= minX && slid + boxW <= plotRight - 4.0f) {
                x = slid;
            }
        }
        if (y + boxH > plotMin.y + plotSz.y - 4.0f) {
            y = std::max(plotMin.y + 6.0f, plotMin.y + plotSz.y - boxH - 4.0f);
        }
        const ImVec2 boxMin(x, y);
        const ImVec2 boxMax(x + boxW, y + boxH);
        dl->AddRectFilled(boxMin, boxMax, IM_COL32(12, 12, 12, 210), 3.0f);
        dl->AddRect(boxMin, boxMax, IM_COL32(255, 255, 255, 40), 3.0f);
        float textY = boxMin.y + 4.0f;
        for (int i = 0; i < nHits && textY + fontPx <= boxMax.y - 2.0f; ++i) {
            if (!hits[i].ok || hits[i].label[0] == '\0') {
                continue;
            }
            if (hits[i].yAxis == ImAxis_Y2) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            } else {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            }
            const ImVec2 pt = ImPlot::PlotToPixels(tMs, hits[i].y);
            if (!std::isfinite(pt.y) || (std::isfinite(mouseY) && std::fabs(pt.y - mouseY) > pickPx)) {
                continue;
            }
            const ImU32 col = ImGui::GetColorU32(hits[i].color);
            const ImVec2 sw(boxMin.x + 6.0f, textY + fontPx * 0.5f);
            dl->AddCircleFilled(sw, 3.5f, col);
            dl->AddText(nullptr, fontPx, ImVec2(boxMin.x + 14.0f, textY), IM_COL32(245, 245, 245, 255),
                        hits[i].label);
            textY += rowH;
        }
    }
    ImPlot::PopPlotClipRect();
}

}  // namespace photon::ui
