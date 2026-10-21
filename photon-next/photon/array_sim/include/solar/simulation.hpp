#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

#include "solar/core.hpp"
#include "solar/trace_scene.hpp"

namespace solar {

struct Sun {
  Vec3 direction_to_sun = normalized({0.25f, -0.18f, 0.951f});
  float dni_w_m2 = 850.0f;
  float dhi_w_m2 = 110.0f;
};

struct SimulationSettings {
  std::uint32_t direct_samples_per_axis = 3;
  std::uint32_t hemisphere_samples = 64;
  std::uint32_t max_reflection_depth = 2;
  float ray_epsilon_m = 1.0e-5f;
  std::uint32_t worker_threads = 0;
};

struct CellGrid {
  std::uint32_t rows = 44;
  std::uint32_t columns = 12;
  float width_m = 0.125f;
  float height_m = 0.125f;
  float active_area_m2 = 0.0153f;
  float gap_m = 0.003f;
  float surface_offset_m = 0.001f;
  float maximum_support_plane_deviation_m = 0.006f;
  float minimum_upward_normal_z = 0.35f;
  float center_x_m = 0.0f;
  float center_y_m = 0.0f;
};

struct Cell {
  std::uint32_t row = 0;
  std::uint32_t column = 0;
  Vec3 center;
  Vec3 normal;
  Vec3 u_axis;
  Vec3 v_axis;
  float width_m = 0.125f;
  float height_m = 0.125f;
  float active_area_m2 = 0.0153f;
  std::uint32_t module_id = 0;
  std::uint32_t module_rows = 0;
  std::uint32_t module_columns = 0;
  float direct_visibility_fraction = 0.0f;
  float direct_w_m2 = 0.0f;
  float diffuse_sky_w_m2 = 0.0f;
  float scene_reflected_w_m2 = 0.0f;

  float total_w_m2() const {
    return direct_w_m2 + diffuse_sky_w_m2 + scene_reflected_w_m2;
  }
};

struct LayoutSummary {
  std::size_t individual_cells = 0;
  std::size_t one_by_three_modules = 0;
  std::size_t two_by_three_modules = 0;
  std::uint32_t longitudinal_phase = 0;
};

struct GeneratedLayout {
  std::vector<Cell> cells;
  LayoutSummary summary;
};

struct SimulationSummary {
  std::size_t cell_count = 0;
  float active_area_m2 = 0.0f;
  float area_weighted_irradiance_w_m2 = 0.0f;
  float incident_solar_power_w = 0.0f;
};

std::vector<Cell> project_cells(const TraceScene& scene, const Bounds& car_bounds,
                                const CellGrid& grid,
                                const ShellRegion* eligible_region = nullptr);

GeneratedLayout generate_vertical_module_layout(
    const std::vector<Cell>& candidates, const CellGrid& grid);

SimulationSummary simulate_cells(std::vector<Cell>& cells, const TraceScene& scene,
                                 const Sun& sun,
                                 const SimulationSettings& settings);

void write_cell_csv(const std::filesystem::path& path,
                    const std::vector<Cell>& cells);

inline Vec3 sun_direction_from_azimuth_elevation(float azimuth_degrees,
                                                 float elevation_degrees) {
  const float azimuth = azimuth_degrees * kPi / 180.0f;
  const float elevation = elevation_degrees * kPi / 180.0f;
  const float horizontal = std::cos(elevation);
  return normalized({std::sin(azimuth) * horizontal,
                     std::cos(azimuth) * horizontal, std::sin(elevation)});
}

}  // namespace solar
