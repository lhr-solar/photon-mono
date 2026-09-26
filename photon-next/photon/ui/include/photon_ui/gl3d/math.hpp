#pragma once

#include <cmath>

namespace photon::ui::gl3d {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

// Column-major 4x4, matching GLES.
struct Mat4 {
    float m[16]{};
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }

inline Vec3 normalize(Vec3 a) {
    const float len = length(a);
    return len > 1e-8f ? a * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
}

inline Vec3 transformPoint(const Mat4& m, Vec3 p) {
    return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
            m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
            m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}

inline Mat4 identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            r.m[c * 4 + row] = a.m[0 * 4 + row] * b.m[c * 4 + 0] + a.m[1 * 4 + row] * b.m[c * 4 + 1] +
                               a.m[2 * 4 + row] * b.m[c * 4 + 2] + a.m[3 * 4 + row] * b.m[c * 4 + 3];
        }
    }
    return r;
}

inline Mat4 translate(Vec3 t) {
    Mat4 r = identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

inline Mat4 scale(Vec3 s) {
    Mat4 r = identity();
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
}

inline Mat4 rotate(float radians, Vec3 axis) {
    const Vec3 n = normalize(axis);
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float ic = 1.0f - c;
    Mat4 r = identity();
    r.m[0] = c + n.x * n.x * ic;
    r.m[1] = n.x * n.y * ic + n.z * s;
    r.m[2] = n.x * n.z * ic - n.y * s;
    r.m[4] = n.y * n.x * ic - n.z * s;
    r.m[5] = c + n.y * n.y * ic;
    r.m[6] = n.y * n.z * ic + n.x * s;
    r.m[8] = n.z * n.x * ic + n.y * s;
    r.m[9] = n.z * n.y * ic - n.x * s;
    r.m[10] = c + n.z * n.z * ic;
    return r;
}

inline Mat4 perspective(float fovyRad, float aspect, float znear, float zfar) {
    Mat4 r{};
    const float f = 1.0f / std::tan(fovyRad * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}

inline Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
    const Vec3 f = normalize(center - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 r = identity();
    r.m[0] = s.x;
    r.m[4] = s.y;
    r.m[8] = s.z;
    r.m[1] = u.x;
    r.m[5] = u.y;
    r.m[9] = u.z;
    r.m[2] = -f.x;
    r.m[6] = -f.y;
    r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

inline void normalMatrix(const Mat4& model, float out9[9]) {
    const float a00 = model.m[0];
    const float a01 = model.m[1];
    const float a02 = model.m[2];
    const float a10 = model.m[4];
    const float a11 = model.m[5];
    const float a12 = model.m[6];
    const float a20 = model.m[8];
    const float a21 = model.m[9];
    const float a22 = model.m[10];
    const float d = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    const float inv = std::fabs(d) > 1e-12f ? 1.0f / d : 1.0f;
    // Inverse-transpose of the upper 3x3.
    out9[0] = (a11 * a22 - a12 * a21) * inv;
    out9[1] = (a02 * a21 - a01 * a22) * inv;
    out9[2] = (a01 * a12 - a02 * a11) * inv;
    out9[3] = (a12 * a20 - a10 * a22) * inv;
    out9[4] = (a00 * a22 - a02 * a20) * inv;
    out9[5] = (a02 * a10 - a00 * a12) * inv;
    out9[6] = (a10 * a21 - a11 * a20) * inv;
    out9[7] = (a01 * a20 - a00 * a21) * inv;
    out9[8] = (a00 * a11 - a01 * a10) * inv;
}

inline constexpr float kPi = 3.14159265358979323846f;

inline float radians(float deg) { return deg * (kPi / 180.0f); }

}  // namespace photon::ui::gl3d
