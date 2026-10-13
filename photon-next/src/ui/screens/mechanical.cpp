#include "ui/screen.hpp"

#include "ui/arena.hpp"
#include "ui/chart_grid.hpp"
#include "ui/client.hpp"
#include "ui/clock_format.hpp"
#include "ui/plot_time.hpp"
#include "ui/scene3d.hpp"
#include "ui/signal_map.hpp"
#include "ui/theme.hpp"
#include "ui/video.hpp"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace photon::ui {
namespace {

bool plotTab(const char* label, bool active) {
    ImGui::PushStyleColor(ImGuiCol_Button, active ? theme::mixColor(theme::background(), theme::foreground(), 0.22f)
                                                : theme::card());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::mixColor(theme::background(), theme::foreground(), 0.28f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::mixColor(theme::background(), theme::foreground(), 0.32f));
    ImGui::PushStyleColor(ImGuiCol_Border, active ? theme::withAlpha(theme::chrome(), 0.65f) : theme::border());
    ImGui::PushStyleColor(ImGuiCol_Text, active ? theme::foreground() : theme::mutedFg());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool clicked = ImGui::Button(label, ImVec2(0, 34));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    return clicked;
}

struct Series {
    std::vector<std::pair<double, double>> gaps;
    std::vector<double> t;
    std::vector<double> v;
};

const SignalRing* ringFor(UiContext& ui, const char* role) {
    const ResolvedScalar* scalar = ui.arena.signalMap().scalar(role);
    return scalar && scalar->mapped ? ui.arena.ring(scalar->id) : nullptr;
}

Series seriesFor(UiContext& ui, const char* role, double start, double end, double scale = 1.0) {
    Series out;
    const ResolvedScalar* scalar = ui.arena.signalMap().scalar(role);
    if (!scalar || !scalar->mapped) return out;
    const auto from = static_cast<std::uint64_t>(std::max(0.0, start));
    const auto to = static_cast<std::uint64_t>(std::max(start, end));
    if (ui.arena.catalogPinned()) {
        const std::uint32_t id = scalar->id;
        ui.arena.tiles().requestWindow(ui.client, std::span<const std::uint32_t>(&id, 1), from, to, 1024,
                                       false);
        const auto plot = ui.arena.tiles().gatherPlot(id, from, to, 1024);
        out.t = plot.t;
        out.v = plot.y;
        out.gaps = plot.gaps;
        for (double& value : out.v) value *= scale;
    } else {
        const SignalRing* ring = ringFor(ui, role);
        if (!ring) return out;
        for (std::size_t i = 0; i < ring->count; ++i) {
            const std::size_t at = ring->index(i);
            const double t = ring->t[at];
            if (t < start || t > end || !std::isfinite(ring->v[at])) continue;
            out.t.push_back(t);
            out.v.push_back(ring->v[at] * scale);
        }
    }
    return out;
}

void plotSeries(const char* label, const Series& s) {
    forEachPlotSegment(s.t, s.gaps, [&](std::size_t begin, std::size_t count) {
        ImPlot::PlotLine(label, s.t.data() + begin, s.v.data() + begin, static_cast<int>(count));
    });
}

void timePlot(const char* id, const char* yLabel, double start, double end,
              const std::vector<std::pair<const char*, Series>>& lines, ImVec2 size) {
    if (!ImPlot::BeginPlot((std::string("##") + id).c_str(), size)) return;
    ImPlot::SetupAxes("Time", yLabel, ImPlotAxisFlags_None, ImPlotAxisFlags_Lock | ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisFormat(ImAxis_X1, formatPlotClock);
    ImPlot::SetupAxisLimits(ImAxis_X1, start, end, ImPlotCond_Once);
    const bool any =
        std::any_of(lines.begin(), lines.end(), [](const auto& entry) { return !entry.second.t.empty(); });
    if (!any) ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 1, ImPlotCond_Always);
    for (const auto& [label, series] : lines) plotSeries(label, series);
    if (!any) ImPlot::PlotText("No samples in selected time window", (start + end) * 0.5, 0.5);
    ImPlot::EndPlot();
}

void histogram(const char* id, const Series& s, ImVec2 size) {
    if (!ImPlot::BeginPlot((std::string("##") + id).c_str(), size)) return;
    ImPlot::SetupAxes("Value", "Count");
    if (!s.v.empty()) {
        ImPlot::PlotHistogram("Samples", s.v.data(), static_cast<int>(s.v.size()), 30);
    } else {
        ImPlot::PlotText("No samples", 0.0, 0.0);
    }
    ImPlot::EndPlot();
}

void status(const char* label, const SignalRing* ring, const char* unit, double scale = 1.0) {
    if (ring && ring->hasLatest && std::isfinite(ring->latest)) {
        ImGui::Text("%s  %.1f %s", label, ring->latest * scale, unit);
    } else {
        ImGui::TextDisabled("%s  --", label);
    }
}

}  // namespace

void MechanicalScreen::draw(UiContext& ui) {
    if (wantDynamics_) hideMaps3d();
    if (!ImGui::BeginTabBar("##mechanical-tabs", ImGuiTabBarFlags_DrawSelectedOverline)) return;
    auto selected = [&](const char* label, bool dynamics = false) {
        const auto flags = dynamics && wantDynamics_ ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        ImGui::PushStyleColor(ImGuiCol_TabSelected, theme::mixColor(theme::background(), theme::foreground(), 0.22f));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, theme::mixColor(theme::background(), theme::foreground(), 0.28f));
        ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, theme::chrome());
        ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 1.0f);
        const bool open = ImGui::BeginTabItem(label, nullptr, flags);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        return open;
    };
    const double end = std::max(15000.0, static_cast<double>(ui.arena.displayCursorMs()));
    const double start = std::max(0.0, end - 15000.0);
    // Data interest belongs to the selected telemetry view, not the hidden tab.
    bool showRaceContext = false;
    auto context = [&](bool dynamics = false) {
        ui.arena.signalMap().subscribe(ui.client,
                                       MapScreen::Pit | MapScreen::Dynamics | MapScreen::Dashboard);
        const bool wide = ImGui::GetContentRegionAvail().x >= 1100.0f;
        auto tiles = [&] {
            showRaceContext = true;
            ui.client.markVideoWanted();
            ui.arena.signalMap().subscribe(ui.client, MapScreen::Scene3d);
            const float width = ImGui::GetContentRegionAvail().x;
            drawScene3DTile(ui.arena, ImVec2(width, 210));
            drawVideoTile(ui.arena, ImVec2(width, 180));
            if (dynamics) {
                dynamics_.drawRaceValues(ui);
            } else {
                status("Speed", ringFor(ui, "vehicle_speed"), "mph", kMpsToMph);
            }
        };
        if (wide) {
            ImGui::BeginChild("##race-context", ImVec2(320, 0), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("RACE CONTEXT");
            tiles();
            ImGui::EndChild();
            ImGui::SameLine();
        } else if (ImGui::CollapsingHeader("Race context"))
            tiles();
        ImGui::BeginChild("##mechanical-charts", ImVec2(0, 0), ImGuiChildFlags_None);
    };
    auto line = [&](const char* id, const char* title, const char* unit, ViewRect rect,
                    std::vector<std::pair<const char*, Series>> lines) -> ChartCard {
        return {id, title, rect, [=](ImVec2 size) { timePlot(id, unit, start, end, lines, size); }};
    };
    auto hist = [&](const char* id, const char* title, ViewRect rect, Series values) -> ChartCard {
        return {id, title, rect, [=](ImVec2 size) { histogram(id, values, size); }};
    };
    if (selected("Driver Input")) {
        context();
        const auto throttle = seriesFor(ui, "throttle", start, end);
        const auto brake = seriesFor(ui, "brake_p1", start, end);
        const auto steer = seriesFor(ui, "steering_angle", start, end);
        static bool distributions = true;
        if (plotTab("Distributions", distributions)) distributions = true;
        ImGui::SameLine();
        if (plotTab("Power vs throttle", !distributions)) distributions = false;
        std::vector<ChartCard> cards;
        cards.push_back(line("controls", "Driver controls", "Input", {0, 0, 24, 14},
                             {{"Throttle %", throttle},
                              {"Brake front psi", brake},
                              {"Brake rear psi", seriesFor(ui, "brake_p2", start, end)},
                              {"Steering deg", steer}}));
        if (distributions) {
            cards.push_back(hist("throttle-hist", "Throttle %", {0, 14, 8, 14}, throttle));
            cards.push_back(hist("brake-hist", "Front brake psi", {8, 14, 8, 14}, brake));
            cards.push_back(hist("steer-hist", "Steering deg", {16, 14, 8, 14}, steer));
        } else {
            cards.push_back(
                {"power-throttle", "Motor power command vs throttle", {0, 14, 24, 14}, [&](ImVec2 size) {
                     const auto power = seriesFor(ui, "power_cmd", start, end);
                     std::vector<double> x, y;
                     PlotArrays pedal;
                     pedal.t = throttle.t;
                     pedal.y = throttle.v;
                     for (std::size_t i = 0; i < power.t.size(); ++i) {
                         double at = 0.0;
                         if (samplePlotArraysAt(pedal, power.t[i], at)) {
                             x.push_back(power.v[i]);
                             y.push_back(at);
                         }
                     }
                     if (ImPlot::BeginPlot("##power-throttle", size)) {
                         ImPlot::SetupAxes("Motor power command %", "Throttle %");
                         if (!x.empty())
                             ImPlot::PlotScatter("Samples", x.data(), y.data(), static_cast<int>(x.size()));
                         else
                             ImPlot::PlotText("No paired samples", 0, 0);
                         ImPlot::EndPlot();
                     }
                 }});
            ImGui::TextDisabled("Motor power command is a setpoint, not measured output power or torque.");
        }
        drawChartBoard(distributions ? "mechanical.driver.distributions" : "mechanical.driver.power", cards);
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (selected("Chassis IMUs")) {
        context();
        drawChartBoard("mechanical.imus", {line("long", "Longitudinal acceleration", "Long g", {0, 0, 12, 14},
                                                {{"Body", seriesFor(ui, "imu_ax", start, end)},
                                                 {"FL", seriesFor(ui, "imu_fl_ax", start, end)},
                                                 {"FR", seriesFor(ui, "imu_fr_ax", start, end)}}),
                                           line("lat", "Lateral acceleration", "Lateral g", {12, 0, 12, 14},
                                                {{"Body", seriesFor(ui, "imu_ay", start, end)},
                                                 {"FL", seriesFor(ui, "imu_fl_ay", start, end)},
                                                 {"FR", seriesFor(ui, "imu_fr_ay", start, end)}}),
                                           hist("lat-hist", "Body lateral g", {0, 14, 12, 14},
                                                seriesFor(ui, "imu_ay", start, end))});
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (selected("Suspension")) {
        context();
        drawChartBoard("mechanical.suspension", {line("travel", "Suspension travel", "mm", {0, 0, 24, 18},
                                                      {{"FL", seriesFor(ui, "susp_fl", start, end)},
                                                       {"FR", seriesFor(ui, "susp_fr", start, end)},
                                                       {"Rear", seriesFor(ui, "susp_rear", start, end)}})});
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (selected("Yaw")) {
        context();
        ImGui::TextDisabled("Front/rear yaw comparison needs calibrated front yaw signals.");
        drawChartBoard("mechanical.yaw", {line("yaw", "Body yaw", "deg/s", {0, 0, 24, 18},
                                               {{"Body yaw", seriesFor(ui, "yaw_rate", start, end)}})});
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (selected("Wheels")) {
        context();
        const auto fl = seriesFor(ui, "wheel_rpm_fl", start, end);
        const auto fr = seriesFor(ui, "wheel_rpm_fr", start, end);
        status("FL", ringFor(ui, "wheel_rpm_fl"), "rpm");
        ImGui::SameLine();
        status("FR", ringFor(ui, "wheel_rpm_fr"), "rpm");
        ImGui::TextDisabled("Rear wheel RPM unavailable");
        drawChartBoard("mechanical.wheels",
                       {line("rpm", "Wheel RPM", "rpm", {0, 0, 24, 14}, {{"FL", fl}, {"FR", fr}}),
                        hist("fl-hist", "FL RPM", {0, 14, 12, 14}, fl),
                        hist("fr-hist", "FR RPM", {12, 14, 12, 14}, fr)});
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (selected("Dynamics", true)) {
        wantDynamics_ = false;
        context(true);
        dynamics_.draw(ui);
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    if (!showRaceContext) hideMaps3d();
}

}  // namespace photon::ui
