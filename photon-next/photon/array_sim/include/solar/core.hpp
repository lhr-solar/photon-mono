#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace solar {

constexpr float kPi = 3.14159265358979323846f;

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  constexpr Vec3() = default;
  constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

  constexpr Vec3 operator+(const Vec3& rhs) const {
    return {x + rhs.x, y + rhs.y, z + rhs.z};
  }
  constexpr Vec3 operator-(const Vec3& rhs) const {
    return {x - rhs.x, y - rhs.y, z - rhs.z};
  }
  constexpr Vec3 operator-() const { return {-x, -y, -z}; }
  constexpr Vec3 operator*(float scalar) const {
    return {x * scalar, y * scalar, z * scalar};
  }
  constexpr Vec3 operator/(float scalar) const {
    return {x / scalar, y / scalar, z / scalar};
  }
  Vec3& operator+=(const Vec3& rhs) {
    x += rhs.x;
    y += rhs.y;
    z += rhs.z;
    return *this;
  }
};

inline constexpr Vec3 operator*(float scalar, const Vec3& value) {
  return value * scalar;
}

inline constexpr float dot(const Vec3& a, const Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline constexpr Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

inline float length(const Vec3& value) { return std::sqrt(dot(value, value)); }

inline Vec3 normalized(const Vec3& value) {
  const float magnitude = length(value);
  if (!(magnitude > 1.0e-12f)) {
    throw std::runtime_error("Cannot normalize a zero-length vector");
  }
  return value / magnitude;
}

inline Vec3 reflected(const Vec3& incoming, const Vec3& normal) {
  return incoming - 2.0f * dot(incoming, normal) * normal;
}

struct Bounds {
  Vec3 minimum{std::numeric_limits<float>::infinity(),
               std::numeric_limits<float>::infinity(),
               std::numeric_limits<float>::infinity()};
  Vec3 maximum{-std::numeric_limits<float>::infinity(),
               -std::numeric_limits<float>::infinity(),
               -std::numeric_limits<float>::infinity()};

  void include(const Vec3& point) {
    minimum.x = std::min(minimum.x, point.x);
    minimum.y = std::min(minimum.y, point.y);
    minimum.z = std::min(minimum.z, point.z);
    maximum.x = std::max(maximum.x, point.x);
    maximum.y = std::max(maximum.y, point.y);
    maximum.z = std::max(maximum.z, point.z);
  }

  Vec3 center() const { return (minimum + maximum) * 0.5f; }
  Vec3 extent() const { return maximum - minimum; }
};

struct Transform {
  float scale = 1.0f;
  Vec3 rotation_degrees{90.0f, 0.0f, 0.0f};
  Vec3 translation{0.0f, -0.6563f, 0.0153f};

  Vec3 apply(Vec3 point) const {
    point = point * scale;
    const float rx = rotation_degrees.x * kPi / 180.0f;
    const float ry = rotation_degrees.y * kPi / 180.0f;
    const float rz = rotation_degrees.z * kPi / 180.0f;
    float c = std::cos(rx), s = std::sin(rx);
    point = {point.x, point.y * c - point.z * s,
             point.y * s + point.z * c};
    c = std::cos(ry);
    s = std::sin(ry);
    point = {point.x * c + point.z * s, point.y,
             -point.x * s + point.z * c};
    c = std::cos(rz);
    s = std::sin(rz);
    point = {point.x * c - point.y * s,
             point.x * s + point.y * c, point.z};
    return point + translation;
  }
};

inline float radical_inverse_base2(std::uint32_t bits) {
  bits = (bits << 16U) | (bits >> 16U);
  bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
  bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
  bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
  bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
  return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

inline Vec3 cosine_hemisphere(std::uint32_t index, std::uint32_t count,
                              const Vec3& normal) {
  const float u = (static_cast<float>(index) + 0.5f) /
                  static_cast<float>(std::max(1U, count));
  const float v = radical_inverse_base2(index);
  const float radius = std::sqrt(u);
  const float phi = 2.0f * kPi * v;
  const float lx = radius * std::cos(phi);
  const float ly = radius * std::sin(phi);
  const float lz = std::sqrt(std::max(0.0f, 1.0f - u));

  const Vec3 helper = std::abs(normal.z) < 0.999f ? Vec3{0.0f, 0.0f, 1.0f}
                                                   : Vec3{0.0f, 1.0f, 0.0f};
  const Vec3 tangent = normalized(cross(helper, normal));
  const Vec3 bitangent = cross(normal, tangent);
  return normalized(tangent * lx + bitangent * ly + normal * lz);
}

inline Vec3 irradiance_color(float irradiance_w_m2) {
  const float t = std::clamp(irradiance_w_m2 / 1000.0f, 0.0f, 1.0f);
  if (t < 0.25f) return {0.04f, 0.22f + 1.6f * t, 0.75f + t};
  if (t < 0.5f) return {0.04f, 0.62f + 0.9f * (t - 0.25f), 1.0f - 2.5f * (t - 0.25f)};
  if (t < 0.75f) return {2.8f * (t - 0.5f), 0.85f, 0.35f - 1.2f * (t - 0.5f)};
  return {0.70f + 1.2f * (t - 0.75f), 0.85f - 2.2f * (t - 0.75f), 0.05f};
}

static_assert(sizeof(Vec3) == 3 * sizeof(float),
              "Vec3 must remain tightly packed for Embree/OpenGL buffers");

}  // namespace solar
