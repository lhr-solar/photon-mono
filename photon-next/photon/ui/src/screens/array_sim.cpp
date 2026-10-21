#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace photon::ui {

void ArraySimScreen::draw(UiContext& ui) {
    ArraySimState& sim = ui.arena.arraySim;
    const bool connected = ui.client.helloOk();
    const bool available = std::find(ui.arena.features.begin(), ui.arena.features.end(), "array_sim") !=
                           ui.arena.features.end();

    ImGui::TextUnformatted("Array Sim");
    ImGui::SameLine();
    ImGui::TextDisabled("stationary, numbers-only ArrayShading run");
    ImGui::Separator();
    ImGui::TextWrapped("Enter the full binary STL path on the computer running photon-backend. "
                       "This run uses the existing upper-shell 2x3/1x3 cell layout and reports incident optical sunlight, not electrical output. "
                       "Sun azimuth is relative to the car's forward (+Y) axis.");

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("Backend STL path", stlPath_, sizeof(stlPath_));
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputFloat("Sun azimuth (deg from car +Y)", &azimuthDeg_, 0.0f, 0.0f, "%.2f");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputFloat("Sun elevation (deg)", &elevationDeg_, 0.0f, 0.0f, "%.2f");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputFloat("DNI (W/m2)", &dniWm2_, 0.0f, 0.0f, "%.1f");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputFloat("DHI (W/m2)", &dhiWm2_, 0.0f, 0.0f, "%.1f");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputInt("Direct samples per cell axis", &directSamples_);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputInt("Hemisphere rays per cell", &hemisphereSamples_);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputInt("Reflection depth", &reflectionDepth_);

    if (!connected) {
        ImGui::TextDisabled("Connect to photon-backend to run Array Sim.");
    } else if (!available) {
        ImGui::TextDisabled("This photon-backend was built without Embree 4; Array Sim is unavailable.");
    }
    const bool valid = stlPath_[0] != '\0' && std::isfinite(azimuthDeg_) &&
                       std::isfinite(elevationDeg_) && elevationDeg_ >= -90.0f && elevationDeg_ <= 90.0f &&
                       std::isfinite(dniWm2_) && dniWm2_ >= 0.0f &&
                       std::isfinite(dhiWm2_) && dhiWm2_ >= 0.0f &&
                       directSamples_ > 0 && directSamples_ <= 64 &&
                       hemisphereSamples_ > 0 && hemisphereSamples_ <= 4096 &&
                       reflectionDepth_ >= 0 && reflectionDepth_ <= 8;
    ImGui::BeginDisabled(!connected || !available || sim.running || !valid);
    if (ImGui::Button(sim.running ? "Running..." : "Run array simulation")) {
        ctrl::Message request;
        request.type = ctrl::Type::ArraySimRequest;
        request.path = stlPath_;
        request.arrayAzimuthDeg = azimuthDeg_;
        request.arrayElevationDeg = elevationDeg_;
        request.arrayDniWm2 = dniWm2_;
        request.arrayDhiWm2 = dhiWm2_;
        request.arrayDirectSamples = static_cast<std::uint32_t>(directSamples_);
        request.arrayHemisphereSamples = static_cast<std::uint32_t>(hemisphereSamples_);
        request.arrayReflectionDepth = static_cast<std::uint32_t>(reflectionDepth_);
        sim.error.clear();
        sim.haveResult = false;
        sim.cells.clear();
        sim.running = ui.client.send(request);
        if (!sim.running) {
            sim.error = "Could not send request to photon-backend";
        }
    }
    ImGui::EndDisabled();
    if (sim.running) {
        ImGui::SameLine();
        ImGui::TextDisabled("Building the STL scene and tracing cells...");
    }
    if (!sim.error.empty()) {
        ImGui::TextWrapped("Error: %s", sim.error.c_str());
    }
    if (!sim.haveResult) {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Area-weighted irradiance: %.2f W/m2", sim.meanIrradianceWm2);
    ImGui::Text("Incident optical power: %.2f W", sim.incidentPowerW);
    ImGui::Text("Active cell area: %.4f m2", sim.activeAreaM2);
    ImGui::Text("Cells: %llu   2x3 modules: %llu   1x3 modules: %llu",
                static_cast<unsigned long long>(sim.cellCount),
                static_cast<unsigned long long>(sim.twoByThreeCount),
                static_cast<unsigned long long>(sim.oneByThreeCount));
    ImGui::TextDisabled("Full STL triangles: %llu", static_cast<unsigned long long>(sim.triangleCount));

    if (ImGui::BeginTable("array-cells", 9,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 300.0f))) {
        ImGui::TableSetupColumn("Module");
        ImGui::TableSetupColumn("Row");
        ImGui::TableSetupColumn("Col");
        ImGui::TableSetupColumn("Direct W/m2");
        ImGui::TableSetupColumn("Diffuse W/m2");
        ImGui::TableSetupColumn("Reflected W/m2");
        ImGui::TableSetupColumn("Total W/m2");
        ImGui::TableSetupColumn("Incident W");
        ImGui::TableSetupColumn("Direct visible");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(sim.cells.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const auto& cell = sim.cells[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", cell.moduleId);
                ImGui::TableNextColumn(); ImGui::Text("%u", cell.row);
                ImGui::TableNextColumn(); ImGui::Text("%u", cell.column);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", cell.directWm2);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", cell.diffuseWm2);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", cell.reflectedWm2);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", cell.totalWm2);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", cell.incidentW);
                ImGui::TableNextColumn(); ImGui::Text("%.0f%%", cell.directVisibility * 100.0);
            }
        }
        ImGui::EndTable();
    }
}

}  // namespace photon::ui
