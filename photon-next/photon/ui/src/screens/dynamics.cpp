#include "photon_ui/client.hpp"
#include "photon_ui/gg_plot.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/theme.hpp"
#include "photon_ui/widgets.hpp"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace photon::ui {
namespace {

constexpr double kWinMs = 15000.0;
constexpr std::size_t kGgMaxPts = 2000;

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

// Newest-first until t < t0, then chronological. O(window), not O(kRingCap).
void extractWindow(const SignalRing& ring, double t0, std::vector<double>& t, std::vector<double>& v) {
    t.clear();
    v.clear();
    if (ring.count == 0) {
        return;
    }
    std::size_t keep = 0;
    for (std::size_t k = 0; k < ring.count; ++k) {
        const std::size_t p = sampleIndex(ring, ring.count - 1 - k);
        if (ring.t[p] < t0) {
            break;
        }
        ++keep;
    }
    t.reserve(keep);
    v.reserve(keep);
    const std::size_t start = ring.count - keep;
    for (std::size_t i = start; i < ring.count; ++i) {
        const std::size_t p = sampleIndex(ring, i);
        if (!std::isfinite(ring.v[p])) {
            continue;
        }
        t.push_back(ring.t[p]);
        v.push_back(ring.v[p]);
    }
}

void pairWindows(const std::vector<double>& at, const std::vector<double>& av, const std::vector<double>& bt,
                 const std::vector<double>& bv, double maxDtMs, std::vector<double>& xa, std::vector<double>& yb) {
    xa.clear();
    yb.clear();
    const std::size_t na = at.size();
    const std::size_t nb = bt.size();
    xa.reserve(nb);
    yb.reserve(nb);
    std::size_t ia = 0;
    for (std::size_t ib = 0; ib < nb; ++ib) {
        const double tb = bt[ib];
        while (ia + 1 < na && at[ia + 1] <= tb) {
            ++ia;
        }
        if (ia >= na) {
            break;
        }
        double dt = std::abs(at[ia] - tb);
        double xv = av[ia];
        if (ia + 1 < na) {
            const double dt2 = std::abs(at[ia + 1] - tb);
            if (dt2 < dt) {
                dt = dt2;
                xv = av[ia + 1];
            }
        }
        if (dt <= maxDtMs) {
            xa.push_back(xv);
            yb.push_back(bv[ib]);
        }
    }
}

void fillSlipWindow(const std::vector<double>& rpmT, const std::vector<double>& rpmV,
                    const std::vector<double>& spdT, const std::vector<double>& spdV, std::vector<double>& t,
                    std::vector<double>& slip) {
    t.clear();
    slip.clear();
    t.reserve(rpmT.size());
    slip.reserve(rpmT.size());
    std::size_t is = 0;
    for (std::size_t i = 0; i < rpmT.size(); ++i) {
        const double tb = rpmT[i];
        while (is + 1 < spdT.size() && spdT[is + 1] <= tb) {
            ++is;
        }
        if (is >= spdT.size()) {
            break;
        }
        double dt = std::abs(spdT[is] - tb);
        double ground = spdV[is];
        if (is + 1 < spdT.size() && std::abs(spdT[is + 1] - tb) < dt) {
            dt = std::abs(spdT[is + 1] - tb);
            ground = spdV[is + 1];
        }
        if (dt > 250.0 || std::abs(ground) < kMinGroundSpeedMps || !std::isfinite(rpmV[i]) ||
            !std::isfinite(ground)) {
            continue;
        }
        t.push_back(tb);
        slip.push_back(slipPercent(wheelSpeedMps(rpmV[i]), ground));
    }
}

// Occupancy cloud: one marker per g-cell. A time-ordered scatter traces the
// car's path and reads as a scribble of connecting lines.
void binGgCloud(const std::vector<double>& x, const std::vector<double>& y, double cell,
                std::vector<double>& ox, std::vector<double>& oy) {
    ox.clear();
    oy.clear();
    if (x.empty() || x.size() != y.size() || cell <= 0.0) {
        return;
    }
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(x.size());
    ox.reserve(std::min(x.size(), static_cast<std::size_t>(512)));
    oy.reserve(ox.capacity());
    const double inv = 1.0 / cell;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) {
            continue;
        }
        const auto ix = static_cast<std::int32_t>(std::floor(x[i] * inv));
        const auto iy = static_cast<std::int32_t>(std::floor(y[i] * inv));
        const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ix)) << 32) |
                         static_cast<std::uint32_t>(iy);
        if (!seen.insert(key).second) {
            continue;
        }
        ox.push_back((static_cast<double>(ix) + 0.5) * cell);
        oy.push_back((static_cast<double>(iy) + 0.5) * cell);
    }
}

void downsampleXy(std::vector<double>& x, std::vector<double>& y, std::size_t maxPts) {
    if (x.size() <= maxPts || maxPts == 0 || x.size() != y.size()) {
        return;
    }
    const std::size_t lastI = x.size() - 1;
    const std::size_t stride = (x.size() + maxPts - 1) / maxPts;
    std::size_t w = 0;
    for (std::size_t i = 0; i < x.size(); i += stride) {
        x[w] = x[i];
        y[w] = y[i];
        ++w;
    }
    if (lastI % stride != 0) {
        x[w] = x[lastI];
        y[w] = y[lastI];
        ++w;
    }
    x.resize(w);
    y.resize(w);
}

void plotLine(const char* name, const std::vector<double>& t, const std::vector<double>& v, std::size_t color) {
    if (t.empty()) {
        return;
    }
    ImPlot::SetNextLineStyle(plotColor(color), theme::plotLineWeight());
    ImPlot::PlotLine(name, t.data(), v.data(), static_cast<int>(t.size()));
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

    imuIndex_ = clampGgImuIndex(imuIndex_);
    const GgImu& imu = kGgImus[imuIndex_];
    const char* tileRoles[] = {"vehicle_speed_mph", "steering_angle", imu.axRole, imu.ayRole,
                               "slip_fl",           "slip_fr",        "yaw_rate", "throttle"};
    for (int i = 0; i < 8; ++i) {
        if (i != 0 && i != 4) {
            ImGui::SameLine(0.0f, gap);
        }
        const Vital v = ui.arena.signalMap().vital(tileRoles[i], ui.arena);
        drawVitalTile(tileRoles[i], v, ImVec2(tileW, tileH));
    }

    const float remainH = std::max(160.0f, avail.y - tileH * 2.0f - gap * 3.0f);
    const float ggSide = std::min(remainH, avail.x * 0.42f);
    const float rightW = std::max(180.0f, avail.x - ggSide - gap);
    const float halfH = (remainH - gap) * 0.5f;

    const SignalMap& map = ui.arena.signalMap();
    const SignalRing* ax = mappedRing(map, ui.arena, imu.axRole);
    const SignalRing* ay = mappedRing(map, ui.arena, imu.ayRole);
    const SignalRing* fl = mappedRing(map, ui.arena, "wheel_rpm_fl");
    const SignalRing* fr = mappedRing(map, ui.arena, "wheel_rpm_fr");
    const SignalRing* spd = mappedRing(map, ui.arena, "vehicle_speed");

    const double nowMs = static_cast<double>(ImGui::GetTime()) * 1000.0;
    const double t1 = windowMax(ax, ay, windowMax(fl, fr, windowMax(spd, nullptr, nowMs)));
    const double t0 = t1 - kWinMs;

    static std::vector<double> axT;
    static std::vector<double> axV;
    static std::vector<double> ayT;
    static std::vector<double> ayV;
    static std::vector<double> flT;
    static std::vector<double> flV;
    static std::vector<double> frT;
    static std::vector<double> frV;
    static std::vector<double> spdT;
    static std::vector<double> spdV;
    static std::vector<double> ggX;
    static std::vector<double> ggY;
    static std::vector<double> ggCloudX;
    static std::vector<double> ggCloudY;
    static std::vector<double> slipTFl;
    static std::vector<double> slipVFl;
    static std::vector<double> slipTFr;
    static std::vector<double> slipVFr;

    if (ax != nullptr) {
        extractWindow(*ax, t0, axT, axV);
    } else {
        axT.clear();
        axV.clear();
    }
    if (ay != nullptr) {
        extractWindow(*ay, t0, ayT, ayV);
    } else {
        ayT.clear();
        ayV.clear();
    }
    if (fl != nullptr) {
        extractWindow(*fl, t0, flT, flV);
    } else {
        flT.clear();
        flV.clear();
    }
    if (fr != nullptr) {
        extractWindow(*fr, t0, frT, frV);
    } else {
        frT.clear();
        frV.clear();
    }
    if (spd != nullptr) {
        extractWindow(*spd, t0, spdT, spdV);
    } else {
        spdT.clear();
        spdV.clear();
    }

    pairWindows(axT, axV, ayT, ayV, 20.0, ggX, ggY);
    GgPeaks windowPeaks{};
    findGgPeaks(ggX, ggY, &windowPeaks);
    static std::string heldKey;
    static GgPeaks heldPeaks;
    const std::string key =
        ggPeakSessionKey(ui.arena.tiles().sessionId(), ui.arena.sourcePath) + "|" + imu.id;
    if (key != heldKey) {
        heldKey = key;
        heldPeaks = {};
    }
    mergeGgPeaks(&heldPeaks, windowPeaks);
    downsampleXy(ggX, ggY, kGgMaxPts);
    binGgCloud(ggX, ggY, 0.05, ggCloudX, ggCloudY);
    fillSlipWindow(flT, flV, spdT, spdV, slipTFl, slipVFl);
    fillSlipWindow(frT, frV, spdT, spdV, slipTFr, slipVFr);

    theme::pushPlotFont();
    const ImVec2 ggMin = ImGui::GetCursorScreenPos();
    if (ImPlot::BeginPlot("##gg", ImVec2(ggSide, ggSide), ImPlotFlags_Equal | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Long G", "Lat G");
        const double span = ggViewSpan(windowPeaks);
        ImPlot::SetupAxesLimits(-span, span, -span, span, ImPlotCond_Always);
        plotCircle("0.5 g", 0.5, theme::withAlpha(theme::mutedFg(), 0.35f));
        plotCircle("1.0 g", 1.0, theme::withAlpha(theme::mutedFg(), 0.55f));
        if (!ggCloudX.empty()) {
            ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 0.0f);
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.5f, plotColor(0), 0.0f, plotColor(0));
            ImPlot::PlotScatter("G-G", ggCloudX.data(), ggCloudY.data(), static_cast<int>(ggCloudX.size()));
            drawGgPeaks(heldPeaks);
            drawGgNow(ggX.back(), ggY.back());
        } else {
            ImPlot::PlotText("no IMU yet", 0.0, 0.0);
        }
        ImPlot::EndPlot();
    }
    ImGui::SetCursorScreenPos(ImVec2(ggMin.x + 8.0f, ggMin.y + 6.0f));
    if (drawGgImuCombo("##gg-imu", &imuIndex_)) {
        heldKey.clear();
        heldPeaks = {};
    }
    ImGui::SetCursorScreenPos(ImVec2(ggMin.x + ggSide + gap, ggMin.y));
    ImGui::BeginGroup();
    if (ImPlot::BeginPlot("##g-time", ImVec2(rightW, halfH))) {
        ImPlot::SetupAxes("t (ms UTC)", "g");
        ImPlot::SetupAxisLimits(ImAxis_X1, t1 - kWinMs, t1, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -2.0, 2.0, ImPlotCond_Once);
        plotLine("Long G", axT, axV, 0);
        plotLine("Lat G", ayT, ayV, 1);
        if (axT.empty() && ayT.empty()) {
            ImPlot::PlotText("no IMU yet", t1 - kWinMs * 0.5, 0.0);
        }
        ImPlot::EndPlot();
    }

    if (ImPlot::BeginPlot("##slip", ImVec2(rightW, halfH))) {
        ImPlot::SetupAxes("t (ms UTC)", "slip %");
        ImPlot::SetupAxisLimits(ImAxis_X1, t1 - kWinMs, t1, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -25.0, 25.0, ImPlotCond_Once);
        plotLine("Slip FL", slipTFl, slipVFl, 0);
        plotLine("Slip FR", slipTFr, slipVFr, 1);
        if (slipTFl.empty() && slipTFr.empty()) {
            ImPlot::PlotText("no slip yet", t1 - kWinMs * 0.5, 0.0);
        }
        ImPlot::EndPlot();
    }
    theme::popPlotFont();
    ImGui::EndGroup();
    ImGui::PopStyleVar();
}

}  // namespace photon::ui
