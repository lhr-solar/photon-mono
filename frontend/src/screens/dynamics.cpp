#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/theme.hpp"
#include "photon_ui/widgets.hpp"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace photon::ui {
namespace {

ImVec4 plotColor(std::size_t i) { return theme::plotSeries(i); }

std::size_t sampleIndex(const SignalRing& ring, std::size_t logicalOldest) {
    if (ring.count < ring.cap) {
        return logicalOldest;
    }
    return (ring.plotOffset() + logicalOldest) % ring.cap;
}

const SignalRing* mappedRing(const SignalMap& map, const ClientArena& arena, const char* role) {
    const ResolvedScalar* slot = map.scalar(role);
    if (slot == nullptr || !slot->mapped || slot->id == 0) {
        return nullptr;
    }
    const SignalRing* ring = arena.ring(slot->id);
    if (ring == nullptr || ring->count == 0) {
        return nullptr;
    }
    return ring;
}

bool nearestAt(const SignalRing& ring, double ts, double maxDtMs, double* value) {
    double bestDt = maxDtMs + 1.0;
    double best = 0.0;
    bool found = false;
    for (std::size_t i = 0; i < ring.count; ++i) {
        const std::size_t p = sampleIndex(ring, i);
        const double dt = std::abs(ring.t[p] - ts);
        if (dt <= maxDtMs && (!found || dt < bestDt)) {
            bestDt = dt;
            best = ring.v[p];
            found = true;
        }
    }
    if (!found || value == nullptr) {
        return found;
    }
    *value = best;
    return true;
}

void pairByTime(const SignalRing& a, const SignalRing& b, double maxDtMs, std::vector<double>& xa,
                std::vector<double>& yb) {
    xa.clear();
    yb.clear();
    std::unordered_map<std::uint64_t, double> byTs;
    byTs.reserve(a.count);
    for (std::size_t i = 0; i < a.count; ++i) {
        const std::size_t p = sampleIndex(a, i);
        if (!std::isfinite(a.v[p])) {
            continue;
        }
        byTs[static_cast<std::uint64_t>(a.t[p])] = a.v[p];
    }
    xa.reserve(b.count);
    yb.reserve(b.count);
    for (std::size_t i = 0; i < b.count; ++i) {
        const std::size_t p = sampleIndex(b, i);
        if (!std::isfinite(b.v[p])) {
            continue;
        }
        const auto it = byTs.find(static_cast<std::uint64_t>(b.t[p]));
        if (it != byTs.end()) {
            xa.push_back(it->second);
            yb.push_back(b.v[p]);
            continue;
        }
        double near = 0.0;
        if (nearestAt(a, b.t[p], maxDtMs, &near)) {
            xa.push_back(near);
            yb.push_back(b.v[p]);
        }
    }
}

void fillSlipSeries(const SignalRing& rpm, const SignalRing& speed, std::vector<double>& t,
                    std::vector<double>& slip) {
    t.clear();
    slip.clear();
    t.reserve(rpm.count);
    slip.reserve(rpm.count);
    for (std::size_t i = 0; i < rpm.count; ++i) {
        const std::size_t p = sampleIndex(rpm, i);
        double ground = 0.0;
        if (!nearestAt(speed, rpm.t[p], 250.0, &ground) || std::abs(ground) < kMinGroundSpeedMps) {
            continue;
        }
        if (!std::isfinite(rpm.v[p]) || !std::isfinite(ground)) {
            continue;
        }
        t.push_back(rpm.t[p]);
        slip.push_back(slipPercent(wheelSpeedMps(rpm.v[p]), ground));
    }
}

void plotRing(const char* name, const SignalRing& ring, std::size_t color) {
    ImPlot::SetNextLineStyle(plotColor(color), theme::plotLineWeight());
    ImPlot::PlotLine(name, ring.t.data(), ring.v.data(), static_cast<int>(ring.count),
                     ImPlotLineFlags_None, static_cast<int>(ring.plotOffset()));
}

void plotCircle(const char* name, double radius, ImVec4 color) {
    constexpr int kN = 65;
    double xs[kN];
    double ys[kN];
    for (int i = 0; i < kN; ++i) {
        const double a = (6.283185307179586 * static_cast<double>(i)) / 64.0;
        xs[i] = radius * std::cos(a);
        ys[i] = radius * std::sin(a);
    }
    ImPlot::SetNextLineStyle(color, 1.2f);
    ImPlot::PlotLine(name, xs, ys, kN);
}

double windowMax(const SignalRing* a, const SignalRing* b, double fallback) {
    double xmax = fallback;
    if (a != nullptr && a->hasLatest) {
        xmax = std::max(xmax, a->latestTs);
    }
    if (b != nullptr && b->hasLatest) {
        xmax = std::max(xmax, b->latestTs);
    }
    return xmax;
}

}  // namespace

void DynamicsScreen::draw(UiContext& ui) {
    ui.arena.signalMap().subscribe(ui.client, MapScreen::Dynamics);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = theme::kGridGap;
    const float tileH = theme::kVitalTileHeight;
    const float tileW = (avail.x - gap * 3.0f) / 4.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));

    static constexpr const char* kTiles[] = {"vehicle_speed_mph", "steering_angle", "imu_ax", "imu_ay",
                                             "slip_fl",           "slip_fr",        "yaw_rate", "throttle"};
    for (int i = 0; i < 8; ++i) {
        if (i != 0 && i != 4) {
            ImGui::SameLine(0.0f, gap);
        }
        const Vital v = ui.arena.signalMap().vital(kTiles[i], ui.arena);
        drawVitalTile(kTiles[i], v, ImVec2(tileW, tileH));
    }

    const float remainH = std::max(160.0f, avail.y - tileH * 2.0f - gap * 3.0f);
    const float ggW = std::min(remainH, avail.x * 0.42f);
    const float rightW = std::max(180.0f, avail.x - ggW - gap);
    const float halfH = (remainH - gap) * 0.5f;

    const SignalMap& map = ui.arena.signalMap();
    const SignalRing* ax = mappedRing(map, ui.arena, "imu_ax");
    const SignalRing* ay = mappedRing(map, ui.arena, "imu_ay");
    const SignalRing* fl = mappedRing(map, ui.arena, "wheel_rpm_fl");
    const SignalRing* fr = mappedRing(map, ui.arena, "wheel_rpm_fr");
    const SignalRing* spd = mappedRing(map, ui.arena, "vehicle_speed");

    std::vector<double> ggX;
    std::vector<double> ggY;
    if (ax != nullptr && ay != nullptr) {
        pairByTime(*ay, *ax, 20.0, ggX, ggY);
    }

    if (ImPlot::BeginPlot("##gg", ImVec2(ggW, remainH), ImPlotFlags_Equal)) {
        ImPlot::SetupAxes("Lat G", "Long G");
        ImPlot::SetupAxesLimits(-2.0, 2.0, -2.0, 2.0, ImPlotCond_Once);
        plotCircle("0.5 g", 0.5, theme::withAlpha(theme::mutedFg(), 0.35f));
        plotCircle("1.0 g", 1.0, theme::withAlpha(theme::mutedFg(), 0.55f));
        if (!ggX.empty()) {
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, plotColor(0), IMPLOT_AUTO,
                                       plotColor(0));
            ImPlot::PlotScatter("G-G", ggX.data(), ggY.data(), static_cast<int>(ggX.size()));
            const double nowX[] = {ggX.back()};
            const double nowY[] = {ggY.back()};
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 8.0f, plotColor(1), IMPLOT_AUTO,
                                       plotColor(1));
            ImPlot::PlotScatter("now", nowX, nowY, 1);
        } else {
            ImPlot::PlotText("no IMU yet", 0.0, 0.0);
        }
        ImPlot::EndPlot();
    }

    ImGui::SameLine(0.0f, gap);
    ImGui::BeginGroup();
    const double nowMs = static_cast<double>(ImGui::GetTime()) * 1000.0;
    const double gMax = windowMax(ax, ay, nowMs);
    if (ImPlot::BeginPlot("##g-time", ImVec2(rightW, halfH))) {
        ImPlot::SetupAxes("t (ms UTC)", "g");
        ImPlot::SetupAxisLimits(ImAxis_X1, gMax - 15000.0, gMax, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -2.0, 2.0, ImPlotCond_Once);
        if (ax != nullptr) {
            plotRing("Long G", *ax, 0);
        }
        if (ay != nullptr) {
            plotRing("Lat G", *ay, 1);
        }
        if (ax == nullptr && ay == nullptr) {
            ImPlot::PlotText("no IMU yet", gMax - 7500.0, 0.0);
        }
        ImPlot::EndPlot();
    }

    std::vector<double> slipTFl;
    std::vector<double> slipVFl;
    std::vector<double> slipTFr;
    std::vector<double> slipVFr;
    if (spd != nullptr && fl != nullptr) {
        fillSlipSeries(*fl, *spd, slipTFl, slipVFl);
    }
    if (spd != nullptr && fr != nullptr) {
        fillSlipSeries(*fr, *spd, slipTFr, slipVFr);
    }
    double slipMax = nowMs;
    if (!slipTFl.empty()) {
        slipMax = std::max(slipMax, slipTFl.back());
    }
    if (!slipTFr.empty()) {
        slipMax = std::max(slipMax, slipTFr.back());
    }
    if (ImPlot::BeginPlot("##slip", ImVec2(rightW, halfH))) {
        ImPlot::SetupAxes("t (ms UTC)", "slip %");
        ImPlot::SetupAxisLimits(ImAxis_X1, slipMax - 15000.0, slipMax, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -25.0, 25.0, ImPlotCond_Once);
        if (!slipTFl.empty()) {
            ImPlot::SetNextLineStyle(plotColor(0), theme::plotLineWeight());
            ImPlot::PlotLine("Slip FL", slipTFl.data(), slipVFl.data(), static_cast<int>(slipTFl.size()));
        }
        if (!slipTFr.empty()) {
            ImPlot::SetNextLineStyle(plotColor(1), theme::plotLineWeight());
            ImPlot::PlotLine("Slip FR", slipTFr.data(), slipVFr.data(), static_cast<int>(slipTFr.size()));
        }
        if (slipTFl.empty() && slipTFr.empty()) {
            ImPlot::PlotText("no slip yet", slipMax - 7500.0, 0.0);
        }
        ImPlot::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::PopStyleVar();
}

}  // namespace photon::ui
