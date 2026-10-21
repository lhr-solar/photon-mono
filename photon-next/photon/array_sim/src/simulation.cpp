#include "solar/simulation.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace solar {
namespace {

std::optional<Hit> top_hit(const TraceScene& scene, float x, float y,
                           float start_z) {
  return scene.intersect({x, y, start_z}, {0.0f, 0.0f, -1.0f}, 1.0e-6f,
                         1.0e30f, kCarMask);
}

std::pair<Vec3, Vec3> tangent_axes(const Vec3& normal) {
  Vec3 preferred{1.0f, 0.0f, 0.0f};
  Vec3 u = preferred - normal * dot(preferred, normal);
  if (length(u) < 1.0e-8f) {
    preferred = {0.0f, 1.0f, 0.0f};
    u = preferred - normal * dot(preferred, normal);
  }
  u = normalized(u);
  return {u, normalized(cross(normal, u))};
}

std::vector<Cell> primary_surface_component(std::vector<Cell> cells,
                                            const CellGrid& grid) {
  if (cells.size() < 2U) return cells;
  std::vector<int> slots(static_cast<std::size_t>(grid.rows) * grid.columns,
                         -1);
  for (std::size_t index = 0; index < cells.size(); ++index) {
    const Cell& cell = cells[index];
    slots[static_cast<std::size_t>(cell.row) * grid.columns + cell.column] =
        static_cast<int>(index);
  }

  std::vector<bool> visited(cells.size(), false);
  std::vector<std::size_t> largest;
  constexpr int kNeighborOffsets[4][2] = {
      {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  constexpr float kMaximumNeighborHeightStepM = 0.10f;
  constexpr float kMinimumNeighborNormalDot = 0.75f;
  for (std::size_t seed = 0; seed < cells.size(); ++seed) {
    if (visited[seed]) continue;
    std::vector<std::size_t> component;
    std::vector<std::size_t> frontier{seed};
    visited[seed] = true;
    while (!frontier.empty()) {
      const std::size_t current_index = frontier.back();
      frontier.pop_back();
      component.push_back(current_index);
      const Cell& current = cells[current_index];
      for (const auto& offset : kNeighborOffsets) {
        const int row = static_cast<int>(current.row) + offset[0];
        const int column = static_cast<int>(current.column) + offset[1];
        if (row < 0 || column < 0 || row >= static_cast<int>(grid.rows) ||
            column >= static_cast<int>(grid.columns))
          continue;
        const int neighbor_slot =
            slots[static_cast<std::size_t>(row) * grid.columns +
                  static_cast<std::uint32_t>(column)];
        if (neighbor_slot < 0) continue;
        const std::size_t neighbor_index =
            static_cast<std::size_t>(neighbor_slot);
        if (visited[neighbor_index]) continue;
        const Cell& neighbor = cells[neighbor_index];
        if (std::abs(current.center.z - neighbor.center.z) >
                kMaximumNeighborHeightStepM ||
            dot(current.normal, neighbor.normal) < kMinimumNeighborNormalDot)
          continue;
        visited[neighbor_index] = true;
        frontier.push_back(neighbor_index);
      }
    }
    if (component.size() > largest.size()) largest = std::move(component);
  }

  std::vector<Cell> result;
  result.reserve(largest.size());
  for (const std::size_t index : largest) result.push_back(cells[index]);
  return result;
}

class Integrator {
 public:
  Integrator(const TraceScene& scene, const Sun& sun,
             const SimulationSettings& settings)
      : scene_(scene), sun_(sun), settings_(settings) {}

  void evaluate(Cell& cell) const {
    cell.direct_visibility_fraction = 0.0f;
    cell.direct_w_m2 = 0.0f;
    cell.diffuse_sky_w_m2 = 0.0f;
    cell.scene_reflected_w_m2 = 0.0f;

    const Vec3 center_origin =
        cell.center + cell.normal * settings_.ray_epsilon_m;
    const float cosine = std::max(0.0f, dot(cell.normal, sun_.direction_to_sun));
    if (cosine > 0.0f) {
      const std::uint32_t axis_samples =
          std::clamp(settings_.direct_samples_per_axis, 1U, 64U);
      std::uint32_t visible_samples = 0;
      for (std::uint32_t v_index = 0; v_index < axis_samples; ++v_index) {
        const float v_fraction =
            (static_cast<float>(v_index) + 0.5f) / axis_samples - 0.5f;
        for (std::uint32_t u_index = 0; u_index < axis_samples; ++u_index) {
          const float u_fraction =
              (static_cast<float>(u_index) + 0.5f) / axis_samples - 0.5f;
          const Vec3 sample =
              cell.center + cell.u_axis * (u_fraction * cell.width_m) +
              cell.v_axis * (v_fraction * cell.height_m);
          if (scene_.visible(sample + cell.normal * settings_.ray_epsilon_m,
                             sun_.direction_to_sun,
                             settings_.ray_epsilon_m)) {
            ++visible_samples;
          }
        }
      }
      const std::uint32_t sample_count = axis_samples * axis_samples;
      cell.direct_visibility_fraction =
          static_cast<float>(visible_samples) / sample_count;
      cell.direct_w_m2 =
          sun_.dni_w_m2 * cosine * cell.direct_visibility_fraction;
    }

    float sky_sum = 0.0f;
    float reflected_sum = 0.0f;
    for (std::uint32_t index = 0; index < settings_.hemisphere_samples; ++index) {
      const Vec3 direction = cosine_hemisphere(
          index, settings_.hemisphere_samples, cell.normal);
      const auto hit = scene_.intersect(center_origin, direction,
                                        settings_.ray_epsilon_m);
      if (!hit) {
        if (direction.z > 0.0f) sky_sum += sun_.dhi_w_m2 / kPi;
      } else {
        reflected_sum += shade_hit(direction, *hit, 0);
      }
    }
    const float weight = kPi / static_cast<float>(settings_.hemisphere_samples);
    cell.diffuse_sky_w_m2 = sky_sum * weight;
    cell.scene_reflected_w_m2 = reflected_sum * weight;
  }

 private:
  float trace_radiance(const Vec3& origin, const Vec3& direction,
                       std::uint32_t depth) const {
    const auto hit = scene_.intersect(origin, direction,
                                      settings_.ray_epsilon_m);
    if (!hit) return direction.z > 0.0f ? sun_.dhi_w_m2 / kPi : 0.0f;
    return shade_hit(direction, *hit, depth);
  }

  float shade_hit(const Vec3& incoming_direction, const Hit& hit,
                  std::uint32_t depth) const {
    const float sun_cosine =
        std::max(0.0f, dot(hit.normal, sun_.direction_to_sun));
    float direct_incident = 0.0f;
    if (sun_cosine > 0.0f &&
        scene_.visible(hit.point + hit.normal * settings_.ray_epsilon_m,
                       sun_.direction_to_sun,
                       settings_.ray_epsilon_m)) {
      direct_incident = sun_.dni_w_m2 * sun_cosine;
    }

    const float sky_view_factor =
        std::clamp((1.0f + hit.normal.z) * 0.5f, 0.0f, 1.0f);
    const float local_incident =
        direct_incident + sun_.dhi_w_m2 * sky_view_factor;
    const float diffuse = hit.material.reflectance *
                          (1.0f - hit.material.specular_fraction) *
                          local_incident / kPi;

    float glossy_sun = 0.0f;
    if (direct_incident > 0.0f && hit.material.specular_fraction > 0.0f) {
      const Vec3 mirror = normalized(reflected(-sun_.direction_to_sun,
                                               hit.normal));
      const Vec3 view = -incoming_direction;
      const float alignment = std::max(0.0f, dot(mirror, view));
      const float phong_brdf =
          hit.material.reflectance * hit.material.specular_fraction *
          (hit.material.specular_exponent + 2.0f) / (2.0f * kPi) *
          std::pow(alignment, hit.material.specular_exponent);
      glossy_sun = phong_brdf * direct_incident;
    }

    if (depth >= settings_.max_reflection_depth ||
        hit.material.specular_fraction <= 0.0f) {
      return diffuse + glossy_sun;
    }
    const Vec3 bounce = normalized(reflected(incoming_direction, hit.normal));
    const float specular =
        hit.material.reflectance * hit.material.specular_fraction *
        trace_radiance(hit.point + bounce * settings_.ray_epsilon_m, bounce,
                       depth + 1U);
    return diffuse + glossy_sun + specular;
  }

  const TraceScene& scene_;
  const Sun& sun_;
  const SimulationSettings& settings_;
};

}  // namespace

std::vector<Cell> project_cells(const TraceScene& scene, const Bounds& bounds,
                                const CellGrid& grid,
                                const ShellRegion* eligible_region) {
  auto inside = [](const Vec3& point, const Vec3& minimum,
                   const Vec3& maximum) {
    return point.x >= minimum.x && point.x <= maximum.x &&
           point.y >= minimum.y && point.y <= maximum.y &&
           point.z >= minimum.z && point.z <= maximum.z;
  };
  const float start_z = bounds.maximum.z + 1.0f;
  const float pitch_x = grid.width_m + grid.gap_m;
  const float pitch_y = grid.height_m + grid.gap_m;
  std::vector<Cell> cells;
  cells.reserve(static_cast<std::size_t>(grid.rows) * grid.columns);
  for (std::uint32_t row = 0; row < grid.rows; ++row) {
    const float y = grid.center_y_m +
                    (static_cast<float>(row) -
                     0.5f * static_cast<float>(grid.rows - 1U)) * pitch_y;
    for (std::uint32_t column = 0; column < grid.columns; ++column) {
      const float x = grid.center_x_m +
                      (static_cast<float>(column) -
                       0.5f * static_cast<float>(grid.columns - 1U)) * pitch_x;
      const auto hit = top_hit(scene, x, y, start_z);
      if (!hit) continue;
      if (eligible_region &&
          (!inside(hit->point, eligible_region->minimum,
                   eligible_region->maximum) ||
           inside(hit->point, eligible_region->exclusion_minimum,
                  eligible_region->exclusion_maximum)))
        continue;
      const float minimum_normal =
          eligible_region
              ? std::max(grid.minimum_upward_normal_z,
                         eligible_region->minimum_upward_normal_z)
              : grid.minimum_upward_normal_z;

      // Fit a planar cell to the local shell tangent. All nine footprint
      // samples must remain close to that plane, which accepts a smooth curved
      // shell but rejects gaps, sharp wheel geometry, and unrelated surfaces.
      bool fully_supported = true;
      std::array<Hit, 9> supports{};
      for (int v_index = -1; v_index <= 1 && fully_supported; ++v_index) {
        for (int u_index = -1; u_index <= 1; ++u_index) {
          const float sample_x =
              x + static_cast<float>(u_index) * 0.5f * grid.width_m;
          const float sample_y =
              y + static_cast<float>(v_index) * 0.5f * grid.height_m;
          const auto support = u_index == 0 && v_index == 0
                                   ? hit
                                   : top_hit(scene, sample_x, sample_y, start_z);
          if (!support ||
              (eligible_region &&
               (!inside(support->point, eligible_region->minimum,
                        eligible_region->maximum) ||
                inside(support->point, eligible_region->exclusion_minimum,
                       eligible_region->exclusion_maximum)))) {
            fully_supported = false;
            break;
          }
          const std::size_t support_index =
              static_cast<std::size_t>((v_index + 1) * 3 + (u_index + 1));
          supports[support_index] = *support;
        }
      }
      if (!fully_supported) continue;

      Vec3 normal = cross(supports[5].point - supports[3].point,
                          supports[7].point - supports[1].point);
      if (length(normal) < 1.0e-8f) continue;
      normal = normalized(normal);
      if (normal.z < 0.0f) normal = -normal;
      if (normal.z < minimum_normal) continue;
      for (const Hit& support : supports) {
        const float plane_deviation =
            std::abs(dot(support.point - hit->point, normal));
        if (support.normal.z < minimum_normal ||
            plane_deviation > grid.maximum_support_plane_deviation_m ||
            dot(support.normal, normal) < 0.75f) {
          fully_supported = false;
          break;
        }
      }
      if (!fully_supported) continue;

      const auto [u, v] = tangent_axes(normal);
      cells.push_back({row,
                       column,
                       hit->point + normal * grid.surface_offset_m,
                       normal,
                       u,
                       v,
                       grid.width_m,
                       grid.height_m,
                       grid.active_area_m2});
    }
  }
  return primary_surface_component(std::move(cells), grid);
}

GeneratedLayout generate_vertical_module_layout(
    const std::vector<Cell>& candidates, const CellGrid& grid) {
  if (grid.rows < 3U || grid.columns == 0U) return {};
  const std::size_t slot_count =
      static_cast<std::size_t>(grid.rows) * grid.columns;
  std::vector<const Cell*> slots(slot_count, nullptr);
  for (const Cell& cell : candidates) {
    if (cell.row < grid.rows && cell.column < grid.columns)
      slots[static_cast<std::size_t>(cell.row) * grid.columns + cell.column] =
          &cell;
  }
  auto at = [&](std::uint32_t row, std::uint32_t column) -> const Cell* {
    return slots[static_cast<std::size_t>(row) * grid.columns + column];
  };
  // Footprint support and local surface continuity were already validated for
  // each candidate. The 1x3/2x3 grouping is an arrangement of conforming
  // cells, not a second rigid plate that must share one normal.
  auto available = [&](std::uint32_t row, std::uint32_t column,
                       std::uint32_t columns) {
    for (std::uint32_t row_offset = 0; row_offset < 3U; ++row_offset) {
      for (std::uint32_t column_offset = 0; column_offset < columns;
           ++column_offset) {
        const Cell* cell = at(row + row_offset, column + column_offset);
        if (!cell) return false;
      }
    }
    return true;
  };

  GeneratedLayout best;
  bool have_best = false;
  for (std::uint32_t phase = 0; phase < 3U; ++phase) {
    GeneratedLayout trial;
    trial.summary.longitudinal_phase = phase;
    std::uint32_t next_module_id = 1U;
    auto append_module = [&](std::uint32_t row, std::uint32_t column,
                             std::uint32_t columns) {
      for (std::uint32_t row_offset = 0; row_offset < 3U; ++row_offset) {
        for (std::uint32_t column_offset = 0; column_offset < columns;
             ++column_offset) {
          Cell selected = *at(row + row_offset, column + column_offset);
          selected.module_id = next_module_id;
          selected.module_rows = 3U;
          selected.module_columns = columns;
          trial.cells.push_back(selected);
        }
      }
      if (columns == 2U)
        ++trial.summary.two_by_three_modules;
      else
        ++trial.summary.one_by_three_modules;
      ++next_module_id;
    };

    for (std::uint32_t row = phase; row + 2U < grid.rows; row += 3U) {
      std::uint32_t column = 0;
      while (column < grid.columns) {
        if (column + 1U < grid.columns && available(row, column, 2U)) {
          append_module(row, column, 2U);
          column += 2U;
        } else if (available(row, column, 1U)) {
          append_module(row, column, 1U);
          ++column;
        } else {
          ++column;
        }
      }
    }
    trial.summary.individual_cells = trial.cells.size();
    const bool better =
        !have_best ||
        trial.summary.two_by_three_modules >
            best.summary.two_by_three_modules ||
        (trial.summary.two_by_three_modules ==
             best.summary.two_by_three_modules &&
         trial.summary.one_by_three_modules >
             best.summary.one_by_three_modules);
    if (better) {
      best = std::move(trial);
      have_best = true;
    }
  }
  return best;
}

SimulationSummary simulate_cells(std::vector<Cell>& cells, const TraceScene& scene,
                                 const Sun& sun,
                                 const SimulationSettings& settings) {
  if (cells.empty()) return {};
  const Integrator integrator(scene, sun, settings);
  const std::uint32_t hardware = std::max(1U, std::thread::hardware_concurrency());
  const std::uint32_t thread_count =
      std::max(1U, settings.worker_threads == 0 ? hardware
                                                : settings.worker_threads);
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (std::uint32_t thread = 0; thread < thread_count; ++thread) {
    workers.emplace_back([&] {
      while (true) {
        const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= cells.size()) break;
        integrator.evaluate(cells[index]);
      }
    });
  }
  for (auto& worker : workers) worker.join();

  SimulationSummary result;
  result.cell_count = cells.size();
  for (const Cell& cell : cells) {
    result.active_area_m2 += cell.active_area_m2;
    result.incident_solar_power_w += cell.total_w_m2() * cell.active_area_m2;
  }
  result.area_weighted_irradiance_w_m2 =
      result.incident_solar_power_w / result.active_area_m2;
  return result;
}

void write_cell_csv(const std::filesystem::path& path,
                    const std::vector<Cell>& cells) {
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) throw std::runtime_error("Cannot write CSV: " + path.string());
  output << "module_id,module_rows,module_columns,row,column,x_m,y_m,z_m,"
            "nx,ny,nz,direct_visibility_fraction,"
            "direct_w_m2,diffuse_sky_w_m2,"
            "scene_reflected_w_m2,total_w_m2,active_area_m2,incident_w\n";
  output << std::setprecision(9);
  for (const Cell& cell : cells) {
    output << cell.module_id << ',' << cell.module_rows << ','
           << cell.module_columns << ',' << cell.row << ',' << cell.column << ','
           << cell.center.x << ','
           << cell.center.y << ',' << cell.center.z << ',' << cell.normal.x
           << ',' << cell.normal.y << ',' << cell.normal.z << ','
           << cell.direct_visibility_fraction << ',' << cell.direct_w_m2 << ','
           << cell.diffuse_sky_w_m2 << ','
           << cell.scene_reflected_w_m2 << ',' << cell.total_w_m2() << ','
           << cell.active_area_m2 << ','
           << cell.total_w_m2() * cell.active_area_m2 << '\n';
  }
}

}  // namespace solar
