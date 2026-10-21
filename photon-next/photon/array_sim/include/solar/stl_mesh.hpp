#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

#include "solar/core.hpp"

namespace solar {

struct TriangleMesh {
  std::vector<Vec3> vertices;
  Bounds bounds;

  std::size_t triangle_count() const { return vertices.size() / 3U; }
};

// World-space placement mask for the current _24-000 car geometry. The main
// box retains the upper shell; the exclusion prism removes the canopy area.
// The complete unfiltered mesh remains the ray-tracing occluder.
struct ShellRegion {
  Vec3 minimum{-0.68f, -2.90f, 0.12f};
  Vec3 maximum{0.68f, 2.80f, 1.00f};
  Vec3 exclusion_minimum{-0.32f, -1.45f, 0.00f};
  Vec3 exclusion_maximum{0.32f, 1.35f, 1.40f};
  float minimum_upward_normal_z = 0.75f;
};

TriangleMesh load_binary_stl(const std::filesystem::path& path,
                             const Transform& transform,
                             std::size_t max_triangles = 0);

TriangleMesh filter_eligible_shell(const TriangleMesh& source,
                                   const ShellRegion& region);

}  // namespace solar
