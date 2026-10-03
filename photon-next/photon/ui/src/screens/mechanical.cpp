#include "photon_ui/screen.hpp"

#include "photon_ui/arena.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/scene3d.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/video.hpp"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace photon::ui {
namespace {

struct Series {
    std::vector<double> t;
    std::vector<double> v;
};

const SignalRing* ringFor(UiContext& ui, const char* role) {
    const ResolvedScalar* scalar = ui.arena.signalMap().scalar(role);
    return scalar && scalar->mapped ? ui.arena.ring(scalar->id) : nullptr;
}

Series seriesFor(UiContext& ui, const char* role, double start, double end, double scale = 1.0) {
    Series out;
    const SignalRing* ring = ringFor(ui, role);
    if (!ring) return out;
    for (std::size_t i = 0; i < ring->count; ++i) {
        const std::size_t at = ring->index(i);
        const double t = ring->t[at];
        if (t < start || t > end || !std::isfinite(ring->v[at])) continue;
        out.t.push_back(t);
        out.v.push_back(ring->v[at] * scale);
    }
    return out;
}

void plotSeries(const char* label, const Series& s) {
    if (!s.t.empty()) ImPlot::PlotLine(label, s.t.data(), s.v.data(), static_cast<int>(s.t.size()));
}

void timePlot(const char* id, const char* yLabel, double start, double end,
              const std::vector<std::pair<const char*, Series>>& lines, float height = 195.0f) {
    if (!ImPlot::BeginPlot(id, ImVec2(-1.0f, height))) return;
    ImPlot::SetupAxes("Time", yLabel);
    ImPlot::SetupAxisLimits(ImAxis_X1, start, end, ImPlotCond_Once);
    bool any = false;
    for (const auto& [label, series] : lines) {
        any |= !series.t.empty();
        plotSeries(label, series);
    }
    if (!any) ImPlot::PlotText("No samples in selected time window", (start + end) * 0.5, 0.0);
    ImPlot::EndPlot();
}

void histogram(const char* id, const Series& s, float height = 155.0f) {
    if (!ImPlot::BeginPlot(id, ImVec2(-1.0f, height))) return;
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
    ui.client.markVideoWanted();
    if (ui.client.helloOk() && !ui.arena.video.subscribed && !ui.arena.catalogPinned()) {
        ui.client.goLive();
    }
    ui.arena.signalMap().subscribe(ui.client, MapScreen::Pit | MapScreen::Dynamics |
                                                  MapScreen::Dashboard | MapScreen::Scene3d);
    const double end = static_cast<double>(ui.arena.displayCursorMs());
    const double start = end - 15000.0;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::clamp(avail.x * 0.31f, 260.0f, 420.0f);

    ImGui::BeginChild("##mechanical-context", ImVec2(side, 0), true);
    ImGui::TextUnformatted("RACE CONTEXT");
    const float tileWidth = std::max(200.0f, ImGui::GetContentRegionAvail().x);
    drawScene3DTile(ui.arena, ImVec2(tileWidth, std::max(170.0f, side * 0.62f)));
    drawVideoTile(ui.arena, ImVec2(tileWidth, std::max(155.0f, side * 0.56f)));
    status("Speed", ringFor(ui, "vehicle_speed"), "mph", kMpsToMph);
    ImGui::TextUnformatted("Driver inputs | 15 s");
    const auto throttle = seriesFor(ui, "throttle", start, end);
    const auto brake = seriesFor(ui, "brake_p1", start, end);
    const auto steer = seriesFor(ui, "steering_angle", start, end);
    timePlot("##mechanical-context-inputs", "Raw units", start, end,
             {{"Throttle %", throttle}, {"Brake psi", brake}, {"Steer deg", steer}}, 130.0f);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##mechanical-workspace", ImVec2(0, 0), false);
    if (ImGui::BeginTabBar("##mechanical-tabs")) {
        if (ImGui::BeginTabItem("Driver Input")) {
            ImGui::TextUnformatted("Throttle, brake pressure, steering and motor command");
            timePlot("##driver-controls", "Input", start, end,
                     {{"Throttle %", throttle}, {"Brake front psi", brake},
                      {"Brake rear psi", seriesFor(ui, "brake_p2", start, end)},
                      {"Steering deg", steer}});
            if (ImGui::BeginTabBar("##driver-detail")) {
                if (ImGui::BeginTabItem("Distributions")) {
                    histogram("Throttle %##hist", throttle);
                    histogram("Front brake psi##hist", brake);
                    histogram("Steering deg##hist", steer);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Power vs throttle")) {
                    const auto power = seriesFor(ui, "power_cmd", start, end);
                    std::vector<double> x, y;
                    const SignalRing* pedal = ringFor(ui, "throttle");
                    if (pedal) {
                        for (std::size_t i = 0; i < power.t.size(); ++i) {
                            double throttleAt = 0.0;
                            if (pedal->interpolate(power.t[i], throttleAt)) {
                                x.push_back(power.v[i]);
                                y.push_back(throttleAt);
                            }
                        }
                    }
                    if (ImPlot::BeginPlot("Motor power command vs throttle", ImVec2(-1, 220))) {
                        ImPlot::SetupAxes("Motor power command %", "Throttle %");
                        if (!x.empty()) ImPlot::PlotScatter("Samples", x.data(), y.data(), static_cast<int>(x.size()));
                        else ImPlot::PlotText("No paired samples", 0.0, 0.0);
                        ImPlot::EndPlot();
                    }
                    ImGui::TextDisabled("Motor power command is a setpoint, not measured output power or torque.");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Chassis IMUs")) {
            ImGui::TextUnformatted("Acceleration at body, front left and front right");
            timePlot("##imu-long", "Long g", start, end,
                     {{"Body", seriesFor(ui, "imu_ax", start, end)},
                      {"FL", seriesFor(ui, "imu_fl_ax", start, end)},
                      {"FR", seriesFor(ui, "imu_fr_ax", start, end)}});
            timePlot("##imu-lat", "Lateral g", start, end,
                     {{"Body", seriesFor(ui, "imu_ay", start, end)},
                      {"FL", seriesFor(ui, "imu_fl_ay", start, end)},
                      {"FR", seriesFor(ui, "imu_fr_ay", start, end)}});
            histogram("Body lateral g##hist", seriesFor(ui, "imu_ay", start, end));
            ImGui::TextDisabled("Body yaw rate is available; wheel IMU yaw and calibrated roll/pitch are pending.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Suspension")) {
            timePlot("##susp-travel", "Travel mm", start, end,
                     {{"FL", seriesFor(ui, "susp_fl", start, end)},
                      {"FR", seriesFor(ui, "susp_fr", start, end)},
                      {"Rear", seriesFor(ui, "susp_rear", start, end)}}, 300.0f);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Yaw")) {
            timePlot("##body-yaw", "deg/s", start, end,
                     {{"Body yaw", seriesFor(ui, "yaw_rate", start, end)}}, 300.0f);
            ImGui::TextDisabled("Front/rear yaw comparison needs calibrated front yaw signals.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Wheels")) {
            const auto fl = seriesFor(ui, "wheel_rpm_fl", start, end);
            const auto fr = seriesFor(ui, "wheel_rpm_fr", start, end);
            status("FL", ringFor(ui, "wheel_rpm_fl"), "rpm");
            status("FR", ringFor(ui, "wheel_rpm_fr"), "rpm");
            ImGui::TextDisabled("Rear wheel RPM unavailable");
            timePlot("##wheel-rpm", "rpm", start, end, {{"FL", fl}, {"FR", fr}}, 280.0f);
            histogram("FL RPM##hist", fl);
            histogram("FR RPM##hist", fr);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

}  // namespace photon::ui
