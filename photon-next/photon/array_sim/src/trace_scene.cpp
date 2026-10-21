#include "solar/trace_scene.hpp"

#include <embree4/rtcore.h>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace solar {
namespace {

struct Index3 {
  std::uint32_t a;
  std::uint32_t b;
  std::uint32_t c;
};

void embree_error(void*, RTCError code, const char* message) {
  std::cerr << "Embree error " << static_cast<int>(code) << ": "
            << (message ? message : "unknown") << '\n';
}

void check_device(RTCDevice device, const char* operation) {
  const RTCError error = rtcGetDeviceError(device);
  if (error != RTC_ERROR_NONE) {
    throw std::runtime_error(std::string(operation) +
                             " failed with Embree error " +
                             std::to_string(static_cast<int>(error)));
  }
}

}  // namespace

struct TraceScene::Impl {
  RTCDevice device = nullptr;
  RTCScene scene = nullptr;
  std::uint32_t car_id = RTC_INVALID_GEOMETRY_ID;
  std::uint32_t ground_id = RTC_INVALID_GEOMETRY_ID;

  ~Impl() {
    if (scene) rtcReleaseScene(scene);
    if (device) rtcReleaseDevice(device);
  }
};

TraceScene::TraceScene(const TriangleMesh& car_mesh)
    : impl_(std::make_unique<Impl>()) {
  impl_->device = rtcNewDevice(nullptr);
  if (!impl_->device) throw std::runtime_error("Could not create Embree device");
  rtcSetDeviceErrorFunction(impl_->device, embree_error, nullptr);
  impl_->scene = rtcNewScene(impl_->device);
  rtcSetSceneBuildQuality(impl_->scene, RTC_BUILD_QUALITY_HIGH);

  RTCGeometry car = rtcNewGeometry(impl_->device, RTC_GEOMETRY_TYPE_TRIANGLE);
  rtcSetGeometryBuildQuality(car, RTC_BUILD_QUALITY_HIGH);
  rtcSetSharedGeometryBuffer(
      car, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
      car_mesh.vertices.data(), 0, sizeof(Vec3), car_mesh.vertices.size());
  auto* indices = static_cast<Index3*>(rtcSetNewGeometryBuffer(
      car, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(Index3),
      car_mesh.triangle_count()));
  for (std::size_t triangle = 0; triangle < car_mesh.triangle_count(); ++triangle) {
    const auto first = static_cast<std::uint32_t>(triangle * 3U);
    indices[triangle] = {first, first + 1U, first + 2U};
  }
  rtcSetGeometryMask(car, kCarMask);
  rtcCommitGeometry(car);
  impl_->car_id = rtcAttachGeometry(impl_->scene, car);
  rtcReleaseGeometry(car);

  RTCGeometry ground = rtcNewGeometry(impl_->device, RTC_GEOMETRY_TYPE_TRIANGLE);
  auto* ground_vertices = static_cast<Vec3*>(rtcSetNewGeometryBuffer(
      ground, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(Vec3), 4));
  ground_vertices[0] = {-20.0f, -20.0f, 0.0f};
  ground_vertices[1] = {20.0f, -20.0f, 0.0f};
  ground_vertices[2] = {-20.0f, 20.0f, 0.0f};
  ground_vertices[3] = {20.0f, 20.0f, 0.0f};
  auto* ground_indices = static_cast<Index3*>(rtcSetNewGeometryBuffer(
      ground, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(Index3), 2));
  ground_indices[0] = {0, 1, 2};
  ground_indices[1] = {1, 3, 2};
  rtcSetGeometryMask(ground, kGroundMask);
  rtcCommitGeometry(ground);
  impl_->ground_id = rtcAttachGeometry(impl_->scene, ground);
  rtcReleaseGeometry(ground);

  rtcCommitScene(impl_->scene);
  check_device(impl_->device, "Scene construction");
}

TraceScene::~TraceScene() = default;
TraceScene::TraceScene(TraceScene&&) noexcept = default;
TraceScene& TraceScene::operator=(TraceScene&&) noexcept = default;

std::optional<Hit> TraceScene::intersect(const Vec3& origin,
                                         const Vec3& direction, float t_near,
                                         float t_far, std::uint32_t mask) const {
  RTCRayHit ray_hit{};
  ray_hit.ray.org_x = origin.x;
  ray_hit.ray.org_y = origin.y;
  ray_hit.ray.org_z = origin.z;
  ray_hit.ray.dir_x = direction.x;
  ray_hit.ray.dir_y = direction.y;
  ray_hit.ray.dir_z = direction.z;
  ray_hit.ray.tnear = t_near;
  ray_hit.ray.tfar = t_far;
  ray_hit.ray.mask = mask;
  ray_hit.ray.flags = 0;
  ray_hit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  ray_hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

  RTCIntersectArguments arguments;
  rtcInitIntersectArguments(&arguments);
  arguments.feature_mask = RTC_FEATURE_FLAG_TRIANGLE;
  rtcIntersect1(impl_->scene, &ray_hit, &arguments);
  if (ray_hit.hit.geomID == RTC_INVALID_GEOMETRY_ID) return std::nullopt;

  Vec3 normal = normalized(
      {ray_hit.hit.Ng_x, ray_hit.hit.Ng_y, ray_hit.hit.Ng_z});
  if (dot(normal, direction) > 0.0f) normal = -normal;
  const Material material = ray_hit.hit.geomID == impl_->ground_id
                                ? Material{0.14f, 0.02f, 20.0f}
                                : Material{0.18f, 0.12f, 60.0f};
  return Hit{ray_hit.ray.tfar,
             origin + direction * ray_hit.ray.tfar,
             normal,
             ray_hit.hit.geomID,
             material};
}

bool TraceScene::visible(const Vec3& origin, const Vec3& direction,
                         float t_near, float t_far, std::uint32_t mask) const {
  RTCRay ray{};
  ray.org_x = origin.x;
  ray.org_y = origin.y;
  ray.org_z = origin.z;
  ray.dir_x = direction.x;
  ray.dir_y = direction.y;
  ray.dir_z = direction.z;
  ray.tnear = t_near;
  ray.tfar = t_far;
  ray.mask = mask;
  ray.flags = 0;
  RTCOccludedArguments arguments;
  rtcInitOccludedArguments(&arguments);
  arguments.feature_mask = RTC_FEATURE_FLAG_TRIANGLE;
  rtcOccluded1(impl_->scene, &ray, &arguments);
  return ray.tfar >= 0.0f;
}

}  // namespace solar
