#include "ui/chart_grid.hpp"
#include "ui/client.hpp"
#include "ui/clock_format.hpp"
#include "ui/gg_plot.hpp"
#include "ui/screen.hpp"
#include "ui/signal_map.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace photon::ui {
namespace {

constexpr double kWinMs = 15000.0;
constexpr std::size_t kGgMaxPts = 2000;

ImVec4 plotColor(std::size_t i) {
    return theme::plotSeries(i);
}

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
                 const std::vector<double>& bv, double maxDtMs, std::vector<double>& xa,
                 std::vector<double>& yb, std::vector<double>& times) {
    xa.clear();
    yb.clear();
    times.clear();
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
            times.push_back(tb);
        }
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

void noteHigh(bool& have, double& slot, std::uint64_t& at, double v, double t) {
    if (!std::isfinite(v)) {
        return;
    }
    if (!have || v > slot) {
        have = true;
        slot = v;
        at = static_cast<std::uint64_t>(t);
    }
}

void noteRange(bool& have, double& hi, double& lo, std::uint64_t& hiAt, std::uint64_t& loAt, double v,
               double t) {
    if (!std::isfinite(v)) {
        return;
    }
    if (!have) {
        have = true;
        hi = lo = v;
        hiAt = loAt = static_cast<std::uint64_t>(t);
        return;
    }
    if (v > hi) {
        hi = v;
        hiAt = static_cast<std::uint64_t>(t);
    }
    if (v < lo) {
        lo = v;
        loAt = static_cast<std::uint64_t>(t);
    }
}

double ringLatest(const SignalRing* ring) {
    if (ring == nullptr || !ring->hasLatest || !std::isfinite(ring->latest)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return ring->latest;
}

void rpmToMph(const std::vector<double>& rpm, std::vector<double>& mph) {
    mph.resize(rpm.size());
    for (std::size_t i = 0; i < rpm.size(); ++i) {
        mph[i] = wheelSpeedMps(rpm[i]) * kMpsToMph;
    }
}

void plotLine(const char* name, const std::vector<double>& t, const std::vector<double>& v,
              std::size_t color) {
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

void DynamicsScreen::drawRaceValues(UiContext& ui) {
    const float gap = theme::kGridGap;
    const float tileW = (ImGui::GetContentRegionAvail().x - gap) / 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const GgImu& imu = kGgImus[clampGgImuIndex(imuIndex_)];
    const char* tileRoles[] = {"vehicle_speed_mph", "steering_angle", imu.axRole,
                               imu.ayRole,          "yaw_rate",       "throttle"};
    for (int i = 0; i < 6; ++i) {
        if (i % 2 != 0) {
            ImGui::SameLine(0.0f, gap);
        }
        const Vital v = ui.arena.signalMap().vital(tileRoles[i], ui.arena);
        drawVitalTile(tileRoles[i], v, ImVec2(tileW, theme::kVitalTileHeight));
    }
    ImGui::PopStyleVar();
}

void DynamicsScreen::draw(UiContext& ui) {
    ui.arena.signalMap().subscribe(ui.client, MapScreen::Dynamics);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kGridGap, theme::kGridGap));
    imuIndex_ = clampGgImuIndex(imuIndex_);
    const GgImu& imu = kGgImus[imuIndex_];

    const ctrl::CatalogSignal* antenna = findAntennaSignal(ui.arena);
    if (antenna != nullptr && antenna->id != 0) {
        ui.client.ensureSubscribed(std::vector<std::uint32_t>{antenna->id});
    }
    theme::pushSmallFont();
    if (antenna != nullptr && ui.arena.has(antenna->id)) {
        ImGui::Text("Antenna  %s  %.4g %s", antenna->name.c_str(), ui.arena.readout(antenna->id),
                    antenna->unit.c_str());
    } else {
        ImGui::TextColored(theme::mutedFg(), "%s", antennaCanNote());
    }
    theme::popSmallFont();

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
    static std::vector<double> ggT;
    static std::vector<double> ggCloudX;
    static std::vector<double> ggCloudY;

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

    pairWindows(axT, axV, ayT, ayV, 20.0, ggX, ggY, ggT);
    GgPeaks windowPeaks{};
    findGgPeaks(ggX, ggY, ggT, &windowPeaks);
    static std::string heldKey;
    static GgPeaks heldPeaks;
    const std::string key =
        ggPeakSessionKey(ui.arena.tiles().sessionId(), ui.arena.sourcePath) + "|" + imu.id;
    if (key != heldKey) {
        heldKey = key;
        heldPeaks = {};
        peakLong_ = false;
        peakLat_ = false;
        peakCombHave_ = false;
        peakWhFlHave_ = false;
        peakWhFrHave_ = false;
    }
    mergeGgPeaks(&heldPeaks, windowPeaks);
    for (std::size_t i = 0; i < axV.size(); ++i) {
        noteRange(peakLong_, peakLongMax_, peakLongMin_, peakLongMaxMs_, peakLongMinMs_, axV[i], axT[i]);
    }
    for (std::size_t i = 0; i < ayV.size(); ++i) {
        noteRange(peakLat_, peakLatMax_, peakLatMin_, peakLatMaxMs_, peakLatMinMs_, ayV[i], ayT[i]);
    }
    if (heldPeaks.combined.have) {
        noteHigh(peakCombHave_, peakComb_, peakCombMs_, heldPeaks.combined.value,
                 static_cast<double>(heldPeaks.combined.timestampMs));
    }
    static std::vector<double> flMph;
    static std::vector<double> frMph;
    rpmToMph(flV, flMph);
    rpmToMph(frV, frMph);
    for (std::size_t i = 0; i < flMph.size(); ++i) {
        noteHigh(peakWhFlHave_, peakWhFl_, peakWhFlMs_, flMph[i], flT[i]);
    }
    for (std::size_t i = 0; i < frMph.size(); ++i) {
        noteHigh(peakWhFrHave_, peakWhFr_, peakWhFrMs_, frMph[i], frT[i]);
    }
    const double nowFl = wheelSpeedMps(ringLatest(fl)) * kMpsToMph;
    const double nowFr = wheelSpeedMps(ringLatest(fr)) * kMpsToMph;
    static std::vector<double> speedGgX[3];
    static std::vector<double> speedGgY[3];
    for (int band = 0; band < 3; ++band) {
        speedGgX[band].clear();
        speedGgY[band].clear();
    }
    if (!spdT.empty() && ggT.size() == ggX.size()) {
        std::size_t speedIndex = 0;
        const std::size_t stride = std::max<std::size_t>(1, (ggX.size() + kGgMaxPts - 1) / kGgMaxPts);
        for (std::size_t i = 0; i < ggX.size(); i += stride) {
            while (speedIndex + 1 < spdT.size() && spdT[speedIndex + 1] <= ggT[i]) ++speedIndex;
            if (std::abs(spdT[speedIndex] - ggT[i]) > 250.0 || !std::isfinite(spdV[speedIndex])) continue;
            const double mph = spdV[speedIndex] * kMpsToMph;
            const int band = mph < 15.0 ? 0 : mph < 35.0 ? 1 : 2;
            speedGgX[band].push_back(ggX[i]);
            speedGgY[band].push_back(ggY[i]);
        }
    }
    downsampleXy(ggX, ggY, kGgMaxPts);
    binGgCloud(ggX, ggY, 0.05, ggCloudX, ggCloudY);

    auto chip = [](const char* text) {
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextUnformatted(text);
    };
    theme::pushSmallFont();
    char chipBuf[64];
    ImGui::TextUnformatted("Peaks");
    std::snprintf(chipBuf, sizeof(chipBuf), "FL %.1f mph", std::isfinite(nowFl) ? nowFl : 0.0);
    chip(std::isfinite(nowFl) ? chipBuf : "FL --");
    std::snprintf(chipBuf, sizeof(chipBuf), "FR %.1f mph", std::isfinite(nowFr) ? nowFr : 0.0);
    chip(std::isfinite(nowFr) ? chipBuf : "FR --");
    std::snprintf(chipBuf, sizeof(chipBuf), "pk FL %.1f", peakWhFlHave_ ? peakWhFl_ : 0.0);
    chip(peakWhFlHave_ ? chipBuf : "pk FL --");
    std::snprintf(chipBuf, sizeof(chipBuf), "pk FR %.1f", peakWhFrHave_ ? peakWhFr_ : 0.0);
    chip(peakWhFrHave_ ? chipBuf : "pk FR --");
    std::snprintf(chipBuf, sizeof(chipBuf), "long %+.2f/%+.2f", peakLong_ ? peakLongMax_ : 0.0,
                  peakLong_ ? peakLongMin_ : 0.0);
    chip(peakLong_ ? chipBuf : "long --");
    std::snprintf(chipBuf, sizeof(chipBuf), "lat %+.2f/%+.2f", peakLat_ ? peakLatMax_ : 0.0,
                  peakLat_ ? peakLatMin_ : 0.0);
    chip(peakLat_ ? chipBuf : "lat --");
    std::snprintf(chipBuf, sizeof(chipBuf), "|g| %.2f", peakCombHave_ ? peakComb_ : 0.0);
    chip(peakCombHave_ ? chipBuf : "|g| --");
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::SmallButton("Reset peaks")) {
        peakLong_ = false;
        peakLat_ = false;
        peakCombHave_ = false;
        peakWhFlHave_ = false;
        peakWhFrHave_ = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Session peaks. Wheel speed uses the 21.56 in rolling diameter on WheelRPM_FL and WheelRPM_FR.");
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::SmallButton("Peak times")) {
        ImGui::OpenPopup("##dynamics-peak-times");
    }
    if (ImGui::BeginPopup("##dynamics-peak-times")) {
        auto row = [](const char* label, bool have, double value, const char* unit, std::uint64_t at) {
            if (!have) return;
            char when[48];
            formatLocalClock(at, when, sizeof(when), ClockParts::TimeMs);
            ImGui::Text("%-12s %+.2f %-4s  %s", label, value, unit, when);
        };
        row("Long +", peakLong_, peakLongMax_, "g", peakLongMaxMs_);
        row("Long -", peakLong_, peakLongMin_, "g", peakLongMinMs_);
        row("Lat +", peakLat_, peakLatMax_, "g", peakLatMaxMs_);
        row("Lat -", peakLat_, peakLatMin_, "g", peakLatMinMs_);
        row("Combined", peakCombHave_, peakComb_, "g", peakCombMs_);
        row("Wheel FL", peakWhFlHave_, peakWhFl_, "mph", peakWhFlMs_);
        row("Wheel FR", peakWhFrHave_, peakWhFr_, "mph", peakWhFrMs_);
        ImGui::EndPopup();
    }
    theme::popSmallFont();

    theme::pushPlotFont();
    if (drawGgImuCombo("##gg-imu", &imuIndex_)) {
        heldKey.clear();
        heldPeaks = {};
    }
    drawChartBoard(
        "mechanical.dynamics",
        {{"gg",
          "G-G",
          {0, 0, 12, 22},
          [&](ImVec2 size) {
              if (ImPlot::BeginPlot("##gg", size, ImPlotFlags_Equal | ImPlotFlags_NoLegend)) {
                  ImPlot::SetupAxes("Long G", "Lat G");
                  const double span = ggViewSpan(windowPeaks);
                  ImPlot::SetupAxesLimits(-span, span, -span, span, ImPlotCond_Once);
                  plotCircle("0.5 g", 0.5, theme::withAlpha(theme::mutedFg(), 0.35f));
                  plotCircle("1.0 g", 1.0, theme::withAlpha(theme::mutedFg(), 0.55f));
                  if (!ggCloudX.empty()) {
                      if (!speedGgX[0].empty() || !speedGgX[1].empty() || !speedGgX[2].empty()) {
                          const char* labels[] = {"<15 mph", "15-35 mph", ">=35 mph"};
                          for (int band = 0; band < 3; ++band) {
                              if (speedGgX[band].empty()) continue;
                              const ImVec4 color = plotColor(static_cast<std::size_t>(band));
                              ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, color, 0.0f, color);
                              ImPlot::PlotScatter(labels[band], speedGgX[band].data(), speedGgY[band].data(),
                                                  static_cast<int>(speedGgX[band].size()));
                          }
                      } else {
                          ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.5f, plotColor(0), 0.0f,
                                                     plotColor(0));
                          ImPlot::PlotScatter("G-G", ggCloudX.data(), ggCloudY.data(),
                                              static_cast<int>(ggCloudX.size()));
                      }
                      drawGgPeaks(heldPeaks);
                      drawGgNow(ggX.back(), ggY.back());
                  } else {
                      ImPlot::PlotText("no IMU yet", 0.0, 0.0);
                  }
                  ImPlot::EndPlot();
              }
          }},
         {"g-time",
          "Acceleration",
          {12, 0, 12, 14},
          [&](ImVec2 size) {
              if (ImPlot::BeginPlot("##g-time", size)) {
                  ImPlot::SetupAxes("Time", "g");
                  ImPlot::SetupAxisFormat(ImAxis_X1, formatPlotClock);
                  ImPlot::SetupAxisLimits(ImAxis_X1, t1 - kWinMs, t1, ImPlotCond_Always);
                  ImPlot::SetupAxisLimits(ImAxis_Y1, -2.0, 2.0, ImPlotCond_Once);
                  plotLine("Long G", axT, axV, 0);
                  plotLine("Lat G", ayT, ayV, 1);
                  if (axT.empty() && ayT.empty()) {
                      ImPlot::PlotText("no IMU yet", t1 - kWinMs * 0.5, 0.0);
                  }
                  ImPlot::EndPlot();
              }
          }},
         {"wheel", "Wheel speed", {12, 14, 12, 14}, [&](ImVec2 size) {
              if (ImPlot::BeginPlot("##wheel", size)) {
                  ImPlot::SetupAxes("Time", "mph");
                  ImPlot::SetupAxisFormat(ImAxis_X1, formatPlotClock);
                  ImPlot::SetupAxisLimits(ImAxis_X1, t1 - kWinMs, t1, ImPlotCond_Always);
                  plotLine("FL", flT, flMph, 0);
                  plotLine("FR", frT, frMph, 1);
                  if (flMph.empty() && frMph.empty()) {
                      ImPlot::PlotText("no wheel speed yet", t1 - kWinMs * 0.5, 0.0);
                  }
                  ImPlot::EndPlot();
              }
          }}});
    theme::popPlotFont();
    ImGui::PopStyleVar();
}

}  // namespace photon::ui
