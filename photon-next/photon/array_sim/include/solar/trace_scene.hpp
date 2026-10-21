#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "solar/core.hpp"
#include "solar/stl_mesh.hpp"

namespace solar {

constexpr std::uint32_t kCarMask = 0x1U;
constexpr std::uint32_t kGroundMask = 0x2U;
constexpr std::uint32_t kAllGeometryMask = kCarMask | kGroundMask;

struct Material {
  float reflectance = 0.18f;
  float specular_fraction = 0.12f;
  float specular_exponent = 60.0f;
};

struct Hit {
  float distance = 0.0f;
  Vec3 point;
  Vec3 normal;
  std::uint32_t geometry_id = 0;
  Material material;
};

class TraceScene {
 public:
  explicit TraceScene(const TriangleMesh& car_mesh);
  ~TraceScene();
  TraceScene(const TraceScene&) = delete;
  TraceScene& operator=(const TraceScene&) = delete;
  TraceScene(TraceScene&&) noexcept;
  TraceScene& operator=(TraceScene&&) noexcept;

  std::optional<Hit> intersect(const Vec3& origin, const Vec3& direction,
                               float t_near = 1.0e-5f,
                               float t_far = 1.0e30f,
                               std::uint32_t mask = kAllGeometryMask) const;
  bool visible(const Vec3& origin, const Vec3& direction,
               float t_near = 1.0e-5f,
               float t_far = 1.0e30f,
               std::uint32_t mask = kAllGeometryMask) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar
