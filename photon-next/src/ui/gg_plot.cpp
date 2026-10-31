#include "ui/gg_plot.hpp"

#include "ui/theme.hpp"

#include "imgui.h"
#include "implot.h"

#include <cmath>

namespace photon::ui {
namespace {

void considerAxis(GgPeak* peak, double metric, double x, double y, std::uint64_t timestampMs, bool greater) {
    if (!peak->have || (greater ? metric > peak->value : metric < peak->value)) {
        peak->have = true;
        peak->x = x;
        peak->y = y;
        peak->value = metric;
        peak->timestampMs = timestampMs;
    }
}

void considerMerge(GgPeak* dst, const GgPeak& src, bool greater) {
    if (!src.have) {
        return;
    }
    considerAxis(dst, src.value, src.x, src.y, src.timestampMs, greater);
}

void drawStick(const char* id, double x1, double y1, ImVec4 col) {
    const double xs[] = {0.0, x1};
    const double ys[] = {0.0, y1};
    ImPlot::SetNextLineStyle(col, 1.9f);
    ImPlot::PlotLine(id, xs, ys, 2);
}

void drawCap(const char* id, double x, double y, bool alongX, ImVec4 col) {
    const double t = 0.055;
    double xs[2];
    double ys[2];
    if (alongX) {
        xs[0] = x;
        xs[1] = x;
        ys[0] = -t;
        ys[1] = t;
    } else {
        xs[0] = -t;
        xs[1] = t;
        ys[0] = y;
        ys[1] = y;
    }
    ImPlot::SetNextLineStyle(col, 2.4f);
    ImPlot::PlotLine(id, xs, ys, 2);
}

void annotate(double x, double y, ImVec4 col, ImVec2 pix, const char* fmt, double value) {
    ImPlot::Annotation(x, y, col, pix, false, fmt, value);
}

}  // namespace

void findGgPeaks(const std::vector<double>& x, const std::vector<double>& y, GgPeaks* out) {
    findGgPeaks(x, y, {}, out);
}

void findGgPeaks(const std::vector<double>& x, const std::vector<double>& y,
                 const std::vector<double>& timestamps, GgPeaks* out) {
    if (out == nullptr) {
        return;
    }
    *out = {};
    if (x.size() != y.size()) {
        return;
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(y[i]) || std::fabs(x[i]) > kGgMaxAbsG ||
            std::fabs(y[i]) > kGgMaxAbsG) {
            continue;
        }
        const std::uint64_t at = i < timestamps.size() && std::isfinite(timestamps[i]) && timestamps[i] > 0.0
                                     ? static_cast<std::uint64_t>(timestamps[i]) : 0;
        considerAxis(&out->longPos, x[i], x[i], y[i], at, true);
        considerAxis(&out->longNeg, x[i], x[i], y[i], at, false);
        considerAxis(&out->latPos, y[i], x[i], y[i], at, true);
        considerAxis(&out->latNeg, y[i], x[i], y[i], at, false);
        considerAxis(&out->combined, std::hypot(x[i], y[i]), x[i], y[i], at, true);
    }
}

void mergeGgPeaks(GgPeaks* dst, const GgPeaks& src) {
    if (dst == nullptr) {
        return;
    }
    considerMerge(&dst->longPos, src.longPos, true);
    considerMerge(&dst->longNeg, src.longNeg, false);
    considerMerge(&dst->latPos, src.latPos, true);
    considerMerge(&dst->latNeg, src.latNeg, false);
    considerMerge(&dst->combined, src.combined, true);
}

double ggPeakSpan(const GgPeaks& peaks, double floor) {
    double span = floor;
    const auto grow = [&](const GgPeak& p, bool axisX) {
        if (!p.have) {
            return;
        }
        const double v = std::fabs(axisX ? p.x : p.y);
        if (v + 0.18 > span) {
            span = v + 0.18;
        }
    };
    grow(peaks.longPos, true);
    grow(peaks.longNeg, true);
    grow(peaks.latPos, false);
    grow(peaks.latNeg, false);
    if (peaks.combined.have && peaks.combined.value + 0.18 > span) {
        span = peaks.combined.value + 0.18;
    }
    if (span > kGgMaxAbsG) {
        span = kGgMaxAbsG;
    }
    return span;
}

double ggViewSpan(const GgPeaks& window, double floor) {
    const double span = ggPeakSpan(window, floor);
    return span > kGgViewMaxG ? kGgViewMaxG : span;
}

void drawGgPeaks(const GgPeaks& peaks) {
    const ImVec4 longCol{0.22f, 0.82f, 0.86f, 1.0f};
    const ImVec4 latCol{1.00f, 0.62f, 0.18f, 1.0f};
    const ImVec4 combCol{0.98f, 0.84f, 0.22f, 1.0f};
    constexpr double kMin = 0.03;

    if (peaks.longPos.have && peaks.longPos.value > kMin) {
        drawStick("##pk-long+", peaks.longPos.value, 0.0, longCol);
        drawCap("##pk-long+c", peaks.longPos.value, 0.0, true, longCol);
        annotate(peaks.longPos.value, 0.0, longCol, ImVec2(8.0f, -4.0f), "%+.2f", peaks.longPos.value);
    }
    if (peaks.longNeg.have && peaks.longNeg.value < -kMin) {
        drawStick("##pk-long-", peaks.longNeg.value, 0.0, longCol);
        drawCap("##pk-long-c", peaks.longNeg.value, 0.0, true, longCol);
        annotate(peaks.longNeg.value, 0.0, longCol, ImVec2(-44.0f, -4.0f), "%+.2f", peaks.longNeg.value);
    }
    if (peaks.latPos.have && peaks.latPos.value > kMin) {
        drawStick("##pk-lat+", 0.0, peaks.latPos.value, latCol);
        drawCap("##pk-lat+c", 0.0, peaks.latPos.value, false, latCol);
        annotate(0.0, peaks.latPos.value, latCol, ImVec2(8.0f, -14.0f), "%+.2f", peaks.latPos.value);
    }
    if (peaks.latNeg.have && peaks.latNeg.value < -kMin) {
        drawStick("##pk-lat-", 0.0, peaks.latNeg.value, latCol);
        drawCap("##pk-lat-c", 0.0, peaks.latNeg.value, false, latCol);
        annotate(0.0, peaks.latNeg.value, latCol, ImVec2(8.0f, 4.0f), "%+.2f", peaks.latNeg.value);
    }
    if (peaks.combined.have && peaks.combined.value > kMin) {
        drawStick("##pk-comb", peaks.combined.x, peaks.combined.y, combCol);
        ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 0.0f);
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Diamond, 7.0f, combCol, 1.4f, theme::foreground());
        const double xs[] = {peaks.combined.x};
        const double ys[] = {peaks.combined.y};
        ImPlot::PlotScatter("##pk-comb-dot", xs, ys, 1);
        annotate(peaks.combined.x, peaks.combined.y, combCol, ImVec2(10.0f, -16.0f), "%.2f g",
                 peaks.combined.value);
    }
}

void drawGgNow(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    const ImVec4 fill{0.98f, 0.42f, 0.70f, 1.0f};
    ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 0.0f);
    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 8.5f, fill, 2.0f, theme::foreground());
    const double xs[] = {x};
    const double ys[] = {y};
    ImPlot::PlotScatter("##now", xs, ys, 1);
}

std::string ggPeakSessionKey(const std::string& sessionId, const std::string& sourcePath) {
    std::string key = sessionId;
    key.push_back('|');
    key.append(sourcePath);
    return key;
}

int clampGgImuIndex(int index) {
    if (index < 0 || index >= kGgImuCount) {
        return 0;
    }
    return index;
}

int ggImuIndexFromMessage(std::string_view message) {
    for (int i = 0; i < kGgImuCount; ++i) {
        if (message == kGgImus[i].message) {
            return i;
        }
    }
    return -1;
}

bool drawGgImuCombo(const char* id, int* index, float width) {
    if (index == nullptr) {
        return false;
    }
    *index = clampGgImuIndex(*index);
    const char* preview = kGgImus[*index].label;
    if (width != 0.0f) {
        ImGui::SetNextItemWidth(width);
    }
    bool changed = false;
    if (ImGui::BeginCombo(id != nullptr ? id : "##gg-imu", preview)) {
        for (int i = 0; i < kGgImuCount; ++i) {
            const bool sel = i == *index;
            if (ImGui::Selectable(kGgImus[i].label, sel)) {
                *index = i;
                changed = true;
            }
            if (sel) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

}  // namespace photon::ui
