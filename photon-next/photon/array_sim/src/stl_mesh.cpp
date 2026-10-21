#include "solar/stl_mesh.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace solar {
namespace {

std::uint32_t little_u32(const unsigned char* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

float little_f32(const unsigned char* bytes) {
  const std::uint32_t bits = little_u32(bytes);
  float result = 0.0f;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

}  // namespace

TriangleMesh load_binary_stl(const std::filesystem::path& path,
                             const Transform& transform,
                             std::size_t max_triangles) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open STL: " + path.string());

  std::array<unsigned char, 84> prefix{};
  input.read(reinterpret_cast<char*>(prefix.data()),
             static_cast<std::streamsize>(prefix.size()));
  if (input.gcount() != static_cast<std::streamsize>(prefix.size())) {
    throw std::runtime_error("STL is shorter than its 84-byte binary header");
  }
  const std::uint32_t declared_count = little_u32(prefix.data() + 80);
  const auto bytes = std::filesystem::file_size(path);
  const std::uintmax_t expected = 84ULL + 50ULL * declared_count;
  if (bytes != expected) {
    throw std::runtime_error(
        "Only binary STL is accepted; file size does not match triangle count");
  }
  const std::size_t count = max_triangles == 0
                                ? declared_count
                                : std::min<std::size_t>(declared_count, max_triangles);
  if (count > std::numeric_limits<std::uint32_t>::max() / 3U) {
    throw std::runtime_error("STL has too many vertices for 32-bit Embree indices");
  }

  TriangleMesh mesh;
  mesh.vertices.reserve(count * 3U);
  std::array<unsigned char, 50> record{};
  for (std::size_t triangle = 0; triangle < count; ++triangle) {
    input.read(reinterpret_cast<char*>(record.data()),
               static_cast<std::streamsize>(record.size()));
    if (!input) throw std::runtime_error("Unexpected EOF in STL triangle data");
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
      const unsigned char* base = record.data() + 12U + vertex * 12U;
      const Vec3 transformed = transform.apply(
          {little_f32(base), little_f32(base + 4), little_f32(base + 8)});
      mesh.vertices.push_back(transformed);
      mesh.bounds.include(transformed);
    }
    if ((triangle + 1U) % 1'000'000U == 0U) {
      std::cout << "  loaded " << (triangle + 1U) << " / " << count
                << " triangles\n";
    }
  }
  if (mesh.vertices.empty()) throw std::runtime_error("STL contains no triangles");
  return mesh;
}

TriangleMesh filter_eligible_shell(const TriangleMesh& source,
                                   const ShellRegion& region) {
  auto inside = [](const Vec3& point, const Vec3& minimum,
                   const Vec3& maximum) {
    return point.x >= minimum.x && point.x <= maximum.x &&
           point.y >= minimum.y && point.y <= maximum.y &&
           point.z >= minimum.z && point.z <= maximum.z;
  };
  if (region.minimum.x > region.maximum.x ||
      region.minimum.y > region.maximum.y ||
      region.minimum.z > region.maximum.z ||
      region.minimum_upward_normal_z < 0.0f ||
      region.minimum_upward_normal_z > 1.0f) {
    throw std::runtime_error("Invalid eligible-shell region");
  }

  TriangleMesh result;
  result.vertices.reserve(source.vertices.size() / 4U);
  for (std::size_t first = 0; first < source.vertices.size(); first += 3U) {
    const Vec3& a = source.vertices[first];
    const Vec3& b = source.vertices[first + 1U];
    const Vec3& c = source.vertices[first + 2U];
    const Vec3 centroid = (a + b + c) / 3.0f;
    if (!inside(centroid, region.minimum, region.maximum)) continue;
    if (inside(centroid, region.exclusion_minimum,
               region.exclusion_maximum))
      continue;
    result.vertices.push_back(a);
    result.vertices.push_back(b);
    result.vertices.push_back(c);
    result.bounds.include(a);
    result.bounds.include(b);
    result.bounds.include(c);
  }
  if (result.vertices.empty())
    throw std::runtime_error(
        "Eligible-shell cutoffs rejected every STL triangle");
  return result;
}

}  // namespace solar
