#include "photon/array_sim.hpp"

#ifdef PHOTON_ARRAY_SIM_EMBREE
#include "solar/simulation.hpp"
#include "solar/stl_mesh.hpp"
#include "solar/trace_scene.hpp"
#endif

#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace photon::array_sim {

bool available() {
#ifdef PHOTON_ARRAY_SIM_EMBREE
    return true;
#else
    return false;
#endif
}

Result run(const Request& request) {
    Result result;
#ifndef PHOTON_ARRAY_SIM_EMBREE
    (void)request;
    result.error = "Array Sim unavailable: backend was built without Embree 4";
    return result;
#else
    try {
        if (request.stlPath.empty()) {
            throw std::runtime_error("Enter an STL path on the backend computer");
        }
        if (!std::isfinite(request.azimuthDeg) || !std::isfinite(request.elevationDeg) ||
            request.elevationDeg < -90.0 || request.elevationDeg > 90.0 ||
            !std::isfinite(request.dniWm2) || request.dniWm2 < 0.0 ||
            !std::isfinite(request.dhiWm2) || request.dhiWm2 < 0.0 ||
            request.directSamplesPerAxis == 0 || request.directSamplesPerAxis > 64 ||
            request.hemisphereSamples == 0 || request.hemisphereSamples > 4096 ||
            request.reflectionDepth > 8) {
            throw std::runtime_error("Invalid sun or ray-sampling parameters");
        }

        // Reuse ArrayShading's complete geometry, placement, and Embree solver.
        solar::TriangleMesh mesh = solar::load_binary_stl(std::filesystem::path(request.stlPath), solar::Transform{});
        result.triangleCount = mesh.triangle_count();
        solar::TraceScene scene(mesh);
        const solar::ShellRegion region;
        solar::TriangleMesh eligibleShell = solar::filter_eligible_shell(mesh, region);
        solar::TraceScene placementScene(eligibleShell);
        const solar::CellGrid grid;
        const auto candidates = solar::project_cells(placementScene, eligibleShell.bounds, grid, &region);
        auto layout = solar::generate_vertical_module_layout(candidates, grid);
        if (layout.cells.empty()) {
            throw std::runtime_error("No cells fit the eligible shell; check STL scale and orientation");
        }

        solar::Sun sun;
        sun.direction_to_sun = solar::sun_direction_from_azimuth_elevation(
            static_cast<float>(request.azimuthDeg), static_cast<float>(request.elevationDeg));
        sun.dni_w_m2 = static_cast<float>(request.dniWm2);
        sun.dhi_w_m2 = static_cast<float>(request.dhiWm2);
        solar::SimulationSettings settings;
        settings.direct_samples_per_axis = request.directSamplesPerAxis;
        settings.hemisphere_samples = request.hemisphereSamples;
        settings.max_reflection_depth = request.reflectionDepth;
        const solar::SimulationSummary summary = solar::simulate_cells(layout.cells, scene, sun, settings);

        result.cellCount = summary.cell_count;
        result.twoByThreeModules = layout.summary.two_by_three_modules;
        result.oneByThreeModules = layout.summary.one_by_three_modules;
        result.activeAreaM2 = summary.active_area_m2;
        result.meanIrradianceWm2 = summary.area_weighted_irradiance_w_m2;
        result.incidentPowerW = summary.incident_solar_power_w;
        result.cells.reserve(layout.cells.size());
        for (const auto& cell : layout.cells) {
            result.cells.push_back(CellResult{cell.row, cell.column, cell.module_id,
                                              cell.direct_w_m2, cell.diffuse_sky_w_m2,
                                              cell.scene_reflected_w_m2, cell.total_w_m2(),
                                              cell.active_area_m2,
                                              cell.total_w_m2() * cell.active_area_m2,
                                              cell.direct_visibility_fraction});
        }
    } catch (const std::exception& ex) {
        result.error = ex.what();
    }
    return result;
#endif
}

}  // namespace photon::array_sim
