#include "photon_ui/gl3d/model.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "tiny_gltf.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace photon::ui::gl3d {
namespace {

std::string lowerCopy(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return o;
}

DynamicPart classifyName(const std::string& name) {
    const std::string n = lowerCopy(name);
    if (n.find("wheel_fl") != std::string::npos || n.find("wheelfl") != std::string::npos) {
        return DynamicPart::WheelFl;
    }
    if (n.find("wheel_fr") != std::string::npos || n.find("wheelfr") != std::string::npos) {
        return DynamicPart::WheelFr;
    }
    if (n.find("wheel_rl") != std::string::npos || n.find("wheelrl") != std::string::npos) {
        return DynamicPart::WheelRl;
    }
    if (n.find("wheel_rr") != std::string::npos || n.find("wheelrr") != std::string::npos ||
        n.find("wheel_rear") != std::string::npos) {
        return DynamicPart::WheelRr;
    }
    if (n.find("body") != std::string::npos) {
        return DynamicPart::Body;
    }
    return DynamicPart::None;
}

// Hard map from dumped new_dyn.glb JSON node names. All 378 nodes are empty_N;
// these are the tire/rim meshes the legacy AABB classifier also hits.
DynamicPart classifyHardName(const std::string& name) {
    if (name == "empty_273" || name == "empty_274") {
        return DynamicPart::WheelFl | DynamicPart::CornerFl;
    }
    if (name == "empty_275" || name == "empty_276") {
        return DynamicPart::WheelFr | DynamicPart::CornerFr;
    }
    if (name == "empty_366" || name == "empty_367" || name == "empty_368" || name == "empty_369" ||
        name == "empty_370" || name == "empty_371") {
        return DynamicPart::WheelRl | DynamicPart::WheelRr | DynamicPart::CornerRear;
    }
    if (name == "empty_188") {
        return DynamicPart::SteeringRack;
    }
    return DynamicPart::None;
}

bool isFastenerNode(const std::string& name) {
    return name == "empty_191" || name == "empty_192" || name == "empty_193" || name == "empty_194" ||
           name == "empty_197" || name == "empty_198";
}

// Spatial classifier ported from Photon/photon/gpu/scene.cpp classifyDynamicParts.
// new_dyn.glb uses glTF Y-up: X = track width, -Z rearward. Link members
// (wishbones / pushrods / tie rods / shocks) are tagged so IK can keep them
// attached to the upright instead of leaving them on the static chassis.
DynamicPart classifySpatial(Vec3 center, Vec3 extent) {
    DynamicPart parts = DynamicPart::None;
    const bool atFrontAxle = center.z > -0.55f && center.z < 0.55f;
    const float lateral = std::fabs(center.x);
    const float frontRadial = std::max(extent.y, extent.z);
    const bool isFrontWheel = atFrontAxle && lateral > 1.62f && frontRadial > 0.85f;
    // Rocker + linpot on the nose deck, including the outboard white lobe that
    // meets the pushrod. That lobe used to classify as pushrod/wishbone and
    // leave the inboard white rocker.
    const bool isShock = atFrontAxle && lateral > 0.50f && lateral < 1.22f && center.y > 1.00f &&
                         center.y < 1.60f && center.z > -0.08f && center.z < 0.18f;
    const bool isPushrod = atFrontAxle && !isShock && lateral > 1.0f && lateral < 1.53f && center.y > 0.42f &&
                           center.y < 1.00f && center.z > 0.025f && center.z < 0.11f;
    const bool isTieRod = atFrontAxle && lateral > 0.55f && lateral < 1.56f && center.y > 0.43f &&
                          center.y < 0.56f && center.z > -0.22f && center.z < -0.07f;
    const bool isUpperWishbone = atFrontAxle && !isPushrod && !isShock && lateral > 0.55f &&
                                 lateral < 1.60f && center.y > 0.82f && center.y < 1.17f &&
                                 !(lateral > 1.56f && center.y < 0.94f);
    const bool isLowerWishbone =
        atFrontAxle && !isTieRod && lateral > 0.55f && lateral < 1.60f && center.y > 0.30f && center.y < 0.43f;
    if (isShock) {
        parts = parts | (center.x < 0.0f ? DynamicPart::ShockFl : DynamicPart::ShockFr);
    } else if (isPushrod) {
        parts = parts | (center.x < 0.0f ? DynamicPart::PushrodFl : DynamicPart::PushrodFr);
    } else if (isTieRod) {
        parts = parts | (center.x < 0.0f ? DynamicPart::TieRodFl : DynamicPart::TieRodFr);
    } else if (isUpperWishbone) {
        parts = parts | (center.x < 0.0f ? DynamicPart::UpperWishboneFl : DynamicPart::UpperWishboneFr);
    } else if (isLowerWishbone) {
        parts = parts | (center.x < 0.0f ? DynamicPart::LowerWishboneFl : DynamicPart::LowerWishboneFr);
    } else if (atFrontAxle && center.x < -1.45f) {
        parts = parts | DynamicPart::CornerFl;
    } else if (atFrontAxle && center.x > 1.45f) {
        parts = parts | DynamicPart::CornerFr;
    }
    if (isFrontWheel) {
        parts = parts | (center.x < 0.0f ? DynamicPart::WheelFl : DynamicPart::WheelFr);
    }
    if (center.z < -4.55f) {
        parts = parts | DynamicPart::CornerRear;
    }
    const float rearRadial = std::max(extent.y, extent.z);
    const float oy = center.y - 0.708f;
    const float oz = center.z + 5.790f;
    const bool onRearAxle = (oy * oy + oz * oz) < 0.02f;
    if (center.z < -5.55f && onRearAxle && rearRadial > 0.22f) {
        parts = parts | DynamicPart::WheelRl | DynamicPart::WheelRr;
    }
    if (std::fabs(center.x) < 0.45f && center.y > 1.60f && center.z < -0.72f && center.z > -1.22f) {
        parts = parts | DynamicPart::SteeringWheel;
    }
    // Long black rack only. Do not tag the column/pinion — sliding those
    // pulls the steering off its chassis mount.
    if (parts == DynamicPart::None && std::fabs(center.x) < 0.20f && center.y > 0.45f && center.y < 0.52f &&
        center.z < -0.10f && center.z > -0.16f && extent.x > 0.80f) {
        parts = DynamicPart::SteeringRack;
    }
    return parts;
}

DynamicPart classifyPart(const std::string& name, Vec3 center, Vec3 extent) {
    const DynamicPart named = classifyName(name);
    if (named != DynamicPart::None) {
        return named;
    }
    const DynamicPart hard = classifyHardName(name);
    if (hard != DynamicPart::None) {
        return hard;
    }
    return classifySpatial(center, extent);
}

void computeLinkEnds(const std::vector<Vertex>& verts, std::uint32_t begin, std::uint32_t count, DrawItem& item) {
    if (count == 0) {
        item.anchor = item.tip = item.pivot;
        return;
    }
    float xMin = 1e9f;
    float xMax = -1e9f;
    for (std::uint32_t i = 0; i < count; ++i) {
        const float x = verts[begin + i].pos[0];
        xMin = std::min(xMin, x);
        xMax = std::max(xMax, x);
    }
    const float range = xMax - xMin;
    const bool left = item.pivot.x < 0.0f;
    const float inboardStart = left ? (xMax - 0.30f * range) : (xMin + 0.30f * range);
    Vec3 inSum{};
    int inN = 0;
    Vec3 bestTip = item.pivot;
    float bestAbs = -1.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        const Vec3 p{verts[begin + i].pos[0], verts[begin + i].pos[1], verts[begin + i].pos[2]};
        const float ax = std::fabs(p.x);
        if (ax > bestAbs) {
            bestAbs = ax;
            bestTip = p;
        }
        const bool inboard = range < 0.04f || (left ? (p.x >= inboardStart) : (p.x <= inboardStart));
        if (inboard) {
            inSum = inSum + p;
            ++inN;
        }
    }
    item.tip = bestTip;
    item.anchor = inN > 0 ? inSum * (1.0f / static_cast<float>(inN)) : item.pivot;
}

void computeLinkEndsIndexed(const std::vector<Vertex>& verts, const std::vector<std::uint32_t>& idx,
                            std::uint32_t first, std::uint32_t count, DrawItem& item) {
    if (count == 0) {
        item.anchor = item.tip = item.pivot;
        return;
    }
    float xMin = 1e9f;
    float xMax = -1e9f;
    for (std::uint32_t i = 0; i < count; ++i) {
        const float x = verts[idx[first + i]].pos[0];
        xMin = std::min(xMin, x);
        xMax = std::max(xMax, x);
    }
    const float range = xMax - xMin;
    const bool left = item.pivot.x < 0.0f;
    const float inboardStart = left ? (xMax - 0.30f * range) : (xMin + 0.30f * range);
    Vec3 inSum{};
    int inN = 0;
    Vec3 bestTip = item.pivot;
    float bestAbs = -1.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        const Vertex& v = verts[idx[first + i]];
        const Vec3 p{v.pos[0], v.pos[1], v.pos[2]};
        const float ax = std::fabs(p.x);
        if (ax > bestAbs) {
            bestAbs = ax;
            bestTip = p;
        }
        const bool inboard = range < 0.04f || (left ? (p.x >= inboardStart) : (p.x <= inboardStart));
        if (inboard) {
            inSum = inSum + p;
            ++inN;
        }
    }
    item.tip = bestTip;
    item.anchor = inN > 0 ? inSum * (1.0f / static_cast<float>(inN)) : item.pivot;
}

Mat4 quatMat(float x, float y, float z, float w) {
    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;
    Mat4 r = identity();
    r.m[0] = 1.0f - 2.0f * (yy + zz);
    r.m[1] = 2.0f * (xy + wz);
    r.m[2] = 2.0f * (xz - wy);
    r.m[4] = 2.0f * (xy - wz);
    r.m[5] = 1.0f - 2.0f * (xx + zz);
    r.m[6] = 2.0f * (yz + wx);
    r.m[8] = 2.0f * (xz + wy);
    r.m[9] = 2.0f * (yz - wx);
    r.m[10] = 1.0f - 2.0f * (xx + yy);
    return r;
}

Mat4 nodeLocal(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        Mat4 r{};
        for (int i = 0; i < 16; ++i) {
            r.m[i] = static_cast<float>(node.matrix[static_cast<std::size_t>(i)]);
        }
        return r;
    }
    Vec3 t{};
    if (node.translation.size() == 3) {
        t = {static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]),
             static_cast<float>(node.translation[2])};
    }
    Vec3 s{1.0f, 1.0f, 1.0f};
    if (node.scale.size() == 3) {
        s = {static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]),
             static_cast<float>(node.scale[2])};
    }
    Mat4 rot = identity();
    if (node.rotation.size() == 4) {
        rot = quatMat(static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]),
                      static_cast<float>(node.rotation[2]), static_cast<float>(node.rotation[3]));
    }
    return translate(t) * rot * scale(s);
}

Vec3 mulPoint(const Mat4& m, Vec3 p) {
    return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
            m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
            m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}

Vec3 mulDir(const Mat4& m, Vec3 p) {
    return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z, m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z,
            m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z};
}

const unsigned char* accessorBytes(const tinygltf::Model& model, const tinygltf::Accessor& acc, std::size_t& stride,
                                   std::size_t& count) {
    if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(model.bufferViews.size())) {
        return nullptr;
    }
    const tinygltf::BufferView& view = model.bufferViews[static_cast<std::size_t>(acc.bufferView)];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size())) {
        return nullptr;
    }
    const tinygltf::Buffer& buf = model.buffers[static_cast<std::size_t>(view.buffer)];
    const std::size_t off = static_cast<std::size_t>(view.byteOffset + acc.byteOffset);
    stride = view.byteStride != 0 ? static_cast<std::size_t>(view.byteStride)
                                  : static_cast<std::size_t>(tinygltf::GetNumComponentsInType(acc.type) *
                                                             tinygltf::GetComponentSizeInBytes(acc.componentType));
    count = static_cast<std::size_t>(acc.count);
    if (off + stride * count > buf.data.size()) {
        return nullptr;
    }
    return buf.data.data() + off;
}

float readFloat(const unsigned char* p, int componentType) {
    if (componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
        float v = 0.0f;
        std::memcpy(&v, p, sizeof(float));
        return v;
    }
    if (componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
        return static_cast<float>(p[0]) / 255.0f;
    }
    if (componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        std::uint16_t v = 0;
        std::memcpy(&v, p, sizeof(std::uint16_t));
        return static_cast<float>(v) / 65535.0f;
    }
    return 0.0f;
}

void appendPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& prim, const Mat4& world,
                     const std::string& nodeName, std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices,
                     std::vector<DrawItem>& draws) {
    if (isFastenerNode(nodeName)) {
        return;
    }
    const auto posIt = prim.attributes.find("POSITION");
    if (posIt == prim.attributes.end()) {
        return;
    }
    const tinygltf::Accessor& posAcc = model.accessors[static_cast<std::size_t>(posIt->second)];
    std::size_t posStride = 0;
    std::size_t posCount = 0;
    const unsigned char* posPtr = accessorBytes(model, posAcc, posStride, posCount);
    if (posPtr == nullptr || posCount == 0) {
        return;
    }

    const tinygltf::Accessor* nrmAcc = nullptr;
    const unsigned char* nrmPtr = nullptr;
    std::size_t nrmStride = 0;
    std::size_t nrmCount = 0;
    const auto nrmIt = prim.attributes.find("NORMAL");
    if (nrmIt != prim.attributes.end()) {
        nrmAcc = &model.accessors[static_cast<std::size_t>(nrmIt->second)];
        nrmPtr = accessorBytes(model, *nrmAcc, nrmStride, nrmCount);
    }

    const tinygltf::Accessor* uvAcc = nullptr;
    const unsigned char* uvPtr = nullptr;
    std::size_t uvStride = 0;
    std::size_t uvCount = 0;
    const auto uvIt = prim.attributes.find("TEXCOORD_0");
    if (uvIt != prim.attributes.end()) {
        uvAcc = &model.accessors[static_cast<std::size_t>(uvIt->second)];
        uvPtr = accessorBytes(model, *uvAcc, uvStride, uvCount);
    }

    const tinygltf::Accessor* colAcc = nullptr;
    const unsigned char* colPtr = nullptr;
    std::size_t colStride = 0;
    std::size_t colCount = 0;
    int colComp = 4;
    const auto colIt = prim.attributes.find("COLOR_0");
    if (colIt != prim.attributes.end()) {
        colAcc = &model.accessors[static_cast<std::size_t>(colIt->second)];
        colPtr = accessorBytes(model, *colAcc, colStride, colCount);
        colComp = tinygltf::GetNumComponentsInType(colAcc->type);
    }

    const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
    Vec3 bmin{1e9f, 1e9f, 1e9f};
    Vec3 bmax{-1e9f, -1e9f, -1e9f};
    verts.reserve(verts.size() + posCount);
    for (std::size_t i = 0; i < posCount; ++i) {
        Vertex v{};
        const int posElem = tinygltf::GetComponentSizeInBytes(posAcc.componentType);
        const float px = readFloat(posPtr + i * posStride + static_cast<std::size_t>(0 * posElem),
                                   posAcc.componentType);
        const float py = readFloat(posPtr + i * posStride + static_cast<std::size_t>(1 * posElem),
                                   posAcc.componentType);
        const float pz = readFloat(posPtr + i * posStride + static_cast<std::size_t>(2 * posElem),
                                   posAcc.componentType);
        const Vec3 wp = mulPoint(world, {px, py, pz});
        v.pos[0] = wp.x;
        v.pos[1] = wp.y;
        v.pos[2] = wp.z;
        Vec3 n{0.0f, 1.0f, 0.0f};
        if (nrmPtr != nullptr && i < nrmCount && nrmAcc != nullptr) {
            const int nrmElem = tinygltf::GetComponentSizeInBytes(nrmAcc->componentType);
            n = {readFloat(nrmPtr + i * nrmStride + static_cast<std::size_t>(0 * nrmElem), nrmAcc->componentType),
                 readFloat(nrmPtr + i * nrmStride + static_cast<std::size_t>(1 * nrmElem), nrmAcc->componentType),
                 readFloat(nrmPtr + i * nrmStride + static_cast<std::size_t>(2 * nrmElem), nrmAcc->componentType)};
            n = normalize(mulDir(world, n));
        }
        v.nrm[0] = n.x;
        v.nrm[1] = n.y;
        v.nrm[2] = n.z;
        if (uvPtr != nullptr && i < uvCount && uvAcc != nullptr) {
            const int uvElem = tinygltf::GetComponentSizeInBytes(uvAcc->componentType);
            v.uv[0] = readFloat(uvPtr + i * uvStride + static_cast<std::size_t>(0 * uvElem),
                                uvAcc->componentType);
            v.uv[1] = readFloat(uvPtr + i * uvStride + static_cast<std::size_t>(1 * uvElem),
                                uvAcc->componentType);
        }
        v.col[0] = v.col[1] = v.col[2] = v.col[3] = 1.0f;
        if (colPtr != nullptr && i < colCount && colAcc != nullptr) {
            for (int c = 0; c < colComp && c < 4; ++c) {
                const int elemSize = tinygltf::GetComponentSizeInBytes(colAcc->componentType);
                v.col[c] = readFloat(colPtr + i * colStride + static_cast<std::size_t>(c * elemSize),
                                     colAcc->componentType);
            }
        }
        bmin.x = std::min(bmin.x, wp.x);
        bmin.y = std::min(bmin.y, wp.y);
        bmin.z = std::min(bmin.z, wp.z);
        bmax.x = std::max(bmax.x, wp.x);
        bmax.y = std::max(bmax.y, wp.y);
        bmax.z = std::max(bmax.z, wp.z);
        verts.push_back(v);
    }

    const std::uint32_t firstIndex = static_cast<std::uint32_t>(indices.size());
    if (prim.indices >= 0 && prim.indices < static_cast<int>(model.accessors.size())) {
        const tinygltf::Accessor& idxAcc = model.accessors[static_cast<std::size_t>(prim.indices)];
        std::size_t idxStride = 0;
        std::size_t idxCount = 0;
        const unsigned char* idxPtr = accessorBytes(model, idxAcc, idxStride, idxCount);
        if (idxPtr == nullptr) {
            return;
        }
        indices.reserve(indices.size() + idxCount);
        for (std::size_t i = 0; i < idxCount; ++i) {
            std::uint32_t value = 0;
            if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                std::memcpy(&value, idxPtr + i * idxStride, sizeof(std::uint32_t));
            } else if (idxAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                std::uint16_t s = 0;
                std::memcpy(&s, idxPtr + i * idxStride, sizeof(std::uint16_t));
                value = s;
            } else {
                value = idxPtr[i * idxStride];
            }
            indices.push_back(base + value);
        }
    } else {
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(posCount); ++i) {
            indices.push_back(base + i);
        }
    }

    DrawItem item{};
    item.firstIndex = firstIndex;
    item.indexCount = static_cast<std::uint32_t>(indices.size()) - firstIndex;
    item.material = prim.material >= 0 ? static_cast<std::uint32_t>(prim.material) : 0u;
    const Vec3 extent = bmax - bmin;
    item.bmin = bmin;
    item.bmax = bmax;
    item.pivot = (bmin + bmax) * 0.5f;
    item.parts = classifyPart(nodeName, item.pivot, extent);
    item.nodeName = nodeName;
    computeLinkEnds(verts, base, static_cast<std::uint32_t>(posCount), item);
    draws.push_back(item);
}

void walkNode(const tinygltf::Model& model, int nodeIndex, const Mat4& parent, std::vector<Vertex>& verts,
              std::vector<std::uint32_t>& indices, std::vector<DrawItem>& draws) {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
        return;
    }
    const tinygltf::Node& node = model.nodes[static_cast<std::size_t>(nodeIndex)];
    const Mat4 world = parent * nodeLocal(node);
    if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size())) {
        const tinygltf::Mesh& mesh = model.meshes[static_cast<std::size_t>(node.mesh)];
        for (const tinygltf::Primitive& prim : mesh.primitives) {
            if (prim.mode >= 0 && prim.mode != TINYGLTF_MODE_TRIANGLES &&
                prim.mode != TINYGLTF_MODE_TRIANGLE_STRIP && prim.mode != TINYGLTF_MODE_TRIANGLE_FAN) {
                continue;
            }
            appendPrimitive(model, prim, world, node.name, verts, indices, draws);
        }
    }
    for (int child : node.children) {
        walkNode(model, child, world, verts, indices, draws);
    }
}

// Bake untextured PBR colors into vertices, then merge draws that share a
// transform (static chassis, same-axle wheels, rear corners). Front corners
// share the legacy steer pivot so they merge too. Drops 378/517 draws to ~8.
void bakeAndBatch(std::vector<Vertex>& verts, std::vector<std::uint32_t>& indices,
                  std::vector<DrawItem>& draws, std::vector<MaterialGpu>& mats) {
    if (draws.empty()) {
        return;
    }
    for (DrawItem& d : draws) {
        if (d.material >= mats.size()) {
            continue;
        }
        const MaterialGpu& m = mats[d.material];
        if (m.hasTexture) {
            continue;
        }
        for (std::uint32_t i = 0; i < d.indexCount; ++i) {
            Vertex& v = verts[indices[d.firstIndex + i]];
            v.col[0] *= m.baseColor.x;
            v.col[1] *= m.baseColor.y;
            v.col[2] *= m.baseColor.z;
            v.col[3] *= m.baseColor.w;
        }
    }

    struct Key {
        std::uint32_t parts = 0;
        unsigned tex = 0;
        bool doubleSided = false;
        bool operator<(const Key& o) const {
            if (parts != o.parts) {
                return parts < o.parts;
            }
            if (tex != o.tex) {
                return tex < o.tex;
            }
            return doubleSided < o.doubleSided;
        }
    };

    // Two-pass: assign each prim to exactly one bucket, then emit that
    // bucket's indices as one contiguous range. A single-pass append (firstIndex
    // at first visit, indexCount = sum) made glDrawElements span interleaved
    // articulated prims — wheels were drawn in the static batch AND again as
    // articulated nodes.
    std::map<Key, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const DrawItem& d = draws[i];
        unsigned tex = 0;
        bool dbl = false;
        if (d.material < mats.size()) {
            const MaterialGpu& m = mats[d.material];
            if (m.hasTexture) {
                tex = m.texture;
            }
            dbl = m.doubleSided;
        }
        groups[{static_cast<std::uint32_t>(d.parts), tex, dbl}].push_back(i);
    }

    std::size_t assigned = 0;
    std::size_t staticPrims = 0;
    std::fprintf(stderr, "[gl3d] prim buckets (%zu prims):\n", draws.size());
    for (const auto& [key, members] : groups) {
        assigned += members.size();
        if (key.parts == 0) {
            staticPrims += members.size();
        }
        std::fprintf(stderr, "  parts=0x%x tex=%u dbl=%d prims=%zu first=%s\n", key.parts, key.tex,
                      static_cast<int>(key.doubleSided), members.size(),
                      members.empty() ? "?" : draws[members.front()].nodeName.c_str());
    }
    if (assigned != draws.size()) {
        std::fprintf(stderr, "[gl3d] ERROR prim assigned to %zu buckets, expected %zu\n", assigned,
                     draws.size());
    }

    std::vector<DrawItem> merged;
    std::vector<std::uint32_t> newIdx;
    newIdx.reserve(indices.size());
    merged.reserve(groups.size());
    for (const auto& [key, members] : groups) {
        DrawItem out = draws[members.front()];
        out.firstIndex = static_cast<std::uint32_t>(newIdx.size());
        out.indexCount = 0;
        if (key.tex == 0) {
            out.material = 0;
        }
        out.nodeName.clear();
        for (std::size_t di : members) {
            const DrawItem& d = draws[di];
            newIdx.insert(newIdx.end(), indices.begin() + static_cast<std::ptrdiff_t>(d.firstIndex),
                          indices.begin() + static_cast<std::ptrdiff_t>(d.firstIndex + d.indexCount));
            out.indexCount += d.indexCount;
            if (!d.nodeName.empty() && out.nodeName.find(d.nodeName) == std::string::npos) {
                if (!out.nodeName.empty()) {
                    out.nodeName += ",";
                }
                out.nodeName += d.nodeName;
            }
        }
        Vec3 bmin{1e9f, 1e9f, 1e9f};
        Vec3 bmax{-1e9f, -1e9f, -1e9f};
        for (std::uint32_t i = 0; i < out.indexCount; ++i) {
            const Vertex& v = verts[newIdx[out.firstIndex + i]];
            bmin.x = std::min(bmin.x, v.pos[0]);
            bmin.y = std::min(bmin.y, v.pos[1]);
            bmin.z = std::min(bmin.z, v.pos[2]);
            bmax.x = std::max(bmax.x, v.pos[0]);
            bmax.y = std::max(bmax.y, v.pos[1]);
            bmax.z = std::max(bmax.z, v.pos[2]);
        }
        if (out.indexCount > 0) {
            out.bmin = bmin;
            out.bmax = bmax;
            out.pivot = (bmin + bmax) * 0.5f;
            computeLinkEndsIndexed(verts, newIdx, out.firstIndex, out.indexCount, out);
        }
        if (hasPart(out.parts, DynamicPart::WheelRl) || hasPart(out.parts, DynamicPart::WheelRr)) {
            std::fprintf(stderr,
                         "[gl3d] rear bucket pivot=(%.3f,%.3f,%.3f) aabb=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f)\n",
                         out.pivot.x, out.pivot.y, out.pivot.z, bmin.x, bmin.y, bmin.z, bmax.x, bmax.y,
                         bmax.z);
        }
        merged.push_back(out);
    }
    if (newIdx.size() != indices.size()) {
        std::fprintf(stderr, "[gl3d] ERROR batched indices %zu != source %zu\n", newIdx.size(),
                     indices.size());
    }
    std::fprintf(stderr, "[gl3d] batched to %zu exclusive draws (static prims %zu, articulated %zu)\n",
                 merged.size(), staticPrims, draws.size() - staticPrims);
    indices.swap(newIdx);
    draws.swap(merged);
}

unsigned uploadImage(Renderer& renderer, const tinygltf::Image& img) {
    if (img.image.empty() || img.width <= 0 || img.height <= 0) {
        return 0;
    }
    std::vector<unsigned char> rgba;
    const int n = img.component;
    const unsigned char* pixels = img.image.data();
    if (n != 4) {
        rgba.resize(static_cast<std::size_t>(img.width * img.height * 4), 255);
        for (int i = 0; i < img.width * img.height; ++i) {
            rgba[static_cast<std::size_t>(i * 4 + 0)] =
                n > 0 ? img.image[static_cast<std::size_t>(i * n + 0)] : 255;
            rgba[static_cast<std::size_t>(i * 4 + 1)] =
                n > 1 ? img.image[static_cast<std::size_t>(i * n + 1)] : 255;
            rgba[static_cast<std::size_t>(i * 4 + 2)] =
                n > 2 ? img.image[static_cast<std::size_t>(i * n + 2)] : 255;
            rgba[static_cast<std::size_t>(i * 4 + 3)] =
                n > 3 ? img.image[static_cast<std::size_t>(i * n + 3)] : 255;
        }
        pixels = rgba.data();
    }
    return renderer.uploadTextureRgba(img.width, img.height, pixels, true);
}

Kinematics defaultKinematics() {
    Kinematics k{};
    auto fill = [](SideKinematics& s, float side) {
        s.upperPivot = {side * 0.88f, 0.93f, 0.0f};
        s.upperTip = {side * 1.50f, 0.98f, 0.0f};
        s.lowerPivot = {side * 0.66f, 0.375f, 0.0f};
        s.lowerTip = {side * 1.50f, 0.40f, 0.0f};
        s.pushAnchor = {side * 1.06f, 1.03f, 0.05f};
        s.pushTip = {side * 1.48f, 0.47f, 0.05f};
        s.tieAnchor = {side * 0.69f, 0.49f, -0.13f};
        s.tieTip = {side * 1.51f, 0.50f, -0.17f};
        s.shockPivot = {side * 0.71f, 1.34f, 0.05f};
        s.wheelPivot = {side * 1.74f, 0.71f, 0.0f};
    };
    fill(k.fl, -1.0f);
    fill(k.fr, 1.0f);
    k.steerPivot = {0.00065f, 1.67644f, -0.94506f};
    k.steerAxis = normalize({0.00018f, 0.42394f, -0.90569f});
    k.rearPivot = {0.0f, 0.708f, -5.790f};
    return k;
}

void overlayCenter(Vec3& dest, const std::vector<const DrawItem*>& items) {
    if (items.empty()) {
        return;
    }
    Vec3 sum{};
    for (const DrawItem* d : items) {
        sum = sum + d->pivot;
    }
    dest = sum * (1.0f / static_cast<float>(items.size()));
}

Kinematics buildKinematics(const std::vector<DrawItem>& draws) {
    // Front attach points stay Photon's measured new_dyn joints. Overlaying AABB
    // centroids moved the rocker pivot onto the linpot body, so the red damper
    // walked off the white rocker instead of staying flush.
    Kinematics kin = defaultKinematics();
    std::vector<const DrawItem*> rearWheels;
    for (const DrawItem& d : draws) {
        if (hasPart(d.parts, DynamicPart::WheelRl) || hasPart(d.parts, DynamicPart::WheelRr)) {
            rearWheels.push_back(&d);
        }
    }
    overlayCenter(kin.rearPivot, rearWheels);
    std::fprintf(stderr, "[gl3d] kin Photon front joints; rear (%+.3f,%+.3f,%+.3f)\n", kin.rearPivot.x,
                 kin.rearPivot.y, kin.rearPivot.z);
    return kin;
}

bool ingestGltf(Renderer& renderer, LoadedModel& out, tinygltf::Model& model) {
    if (model.scenes.empty()) {
        out.error = "glTF has no scenes";
        return false;
    }
    const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
    if (sceneIndex >= static_cast<int>(model.scenes.size())) {
        out.error = "glTF default scene out of range";
        return false;
    }
    std::vector<Vertex> verts;
    std::vector<std::uint32_t> indices;
    for (int node : model.scenes[static_cast<std::size_t>(sceneIndex)].nodes) {
        walkNode(model, node, identity(), verts, indices, out.gpu.draws);
    }
    if (verts.empty() || indices.empty()) {
        out.error = "glTF produced no triangles";
        return false;
    }

    Vec3 bmin{1e9f, 1e9f, 1e9f};
    Vec3 bmax{-1e9f, -1e9f, -1e9f};
    for (const Vertex& v : verts) {
        bmin.x = std::min(bmin.x, v.pos[0]);
        bmin.y = std::min(bmin.y, v.pos[1]);
        bmin.z = std::min(bmin.z, v.pos[2]);
        bmax.x = std::max(bmax.x, v.pos[0]);
        bmax.y = std::max(bmax.y, v.pos[1]);
        bmax.z = std::max(bmax.z, v.pos[2]);
    }
    out.aabbMin = bmin;
    out.aabbMax = bmax;
    out.haveAabb = true;
    const Vec3 extent = bmax - bmin;
    out.radius = length(extent) * 0.5f;
    std::fprintf(stderr, "[gl3d] %s bounds min=(%.3f,%.3f,%.3f) max=(%.3f,%.3f,%.3f) radius=%.3f\n",
                 out.name.empty() ? "?" : out.name.c_str(), bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z,
                 out.radius);

    std::vector<unsigned> imageTex(model.images.size(), 0);
    for (const tinygltf::Material& mat : model.materials) {
        MaterialGpu gpu{};
        gpu.doubleSided = mat.doubleSided;
        const auto& pbr = mat.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() >= 3) {
            gpu.baseColor.x = static_cast<float>(pbr.baseColorFactor[0]);
            gpu.baseColor.y = static_cast<float>(pbr.baseColorFactor[1]);
            gpu.baseColor.z = static_cast<float>(pbr.baseColorFactor[2]);
            gpu.baseColor.w = pbr.baseColorFactor.size() >= 4 ? static_cast<float>(pbr.baseColorFactor[3]) : 1.0f;
        }
        const int texIndex = pbr.baseColorTexture.index;
        if (texIndex >= 0 && texIndex < static_cast<int>(model.textures.size())) {
            const int src = model.textures[static_cast<std::size_t>(texIndex)].source;
            if (src >= 0 && src < static_cast<int>(model.images.size())) {
                if (imageTex[static_cast<std::size_t>(src)] == 0) {
                    imageTex[static_cast<std::size_t>(src)] =
                        uploadImage(renderer, model.images[static_cast<std::size_t>(src)]);
                    if (imageTex[static_cast<std::size_t>(src)] != 0) {
                        out.gpu.textures.push_back(imageTex[static_cast<std::size_t>(src)]);
                    }
                }
                gpu.texture = imageTex[static_cast<std::size_t>(src)];
                gpu.hasTexture = gpu.texture != 0;
            }
        }
        out.gpu.materials.push_back(gpu);
    }
    if (out.gpu.materials.empty()) {
        out.gpu.materials.push_back(MaterialGpu{});
    }

    // newCar.glb is one node (empty_195) with 517 prims — per-prim AABBs would
    // false-hit the wheel classifier. Force rigid display.
    const bool namedWheel =
        std::any_of(out.gpu.draws.begin(), out.gpu.draws.end(),
                    [](const DrawItem& d) { return classifyName(d.nodeName) != DynamicPart::None; });
    std::string uniqueNode;
    bool oneNode = true;
    for (const DrawItem& d : out.gpu.draws) {
        if (d.nodeName.empty()) {
            continue;
        }
        if (uniqueNode.empty()) {
            uniqueNode = d.nodeName;
        } else if (d.nodeName != uniqueNode) {
            oneNode = false;
            break;
        }
    }
    out.rigid = oneNode && !namedWheel && !out.gpu.draws.empty();
    if (out.rigid) {
        for (DrawItem& d : out.gpu.draws) {
            d.parts = DynamicPart::None;
        }
        out.kin = Kinematics{};
    } else {
        out.kin = buildKinematics(out.gpu.draws);
    }

    out.triangleCount = static_cast<std::uint32_t>(indices.size() / 3);
    summarizeBinds(out);
    bakeAndBatch(verts, indices, out.gpu.draws, out.gpu.materials);
    renderer.uploadMesh(out.gpu.mesh, verts.data(), verts.size(), indices.data(), indices.size());
    out.gpu.loaded = true;
    out.fromGltf = true;
    return true;
}

}  // namespace

void summarizeBinds(LoadedModel& model) {
    model.binds = BindStats{};
    for (const DrawItem& item : model.gpu.draws) {
        if (hasPart(item.parts, DynamicPart::WheelFl)) {
            ++model.binds.wheelsFl;
        }
        if (hasPart(item.parts, DynamicPart::WheelFr)) {
            ++model.binds.wheelsFr;
        }
        if (hasPart(item.parts, DynamicPart::WheelRl)) {
            ++model.binds.wheelsRl;
        }
        if (hasPart(item.parts, DynamicPart::WheelRr)) {
            ++model.binds.wheelsRr;
        }
        if (hasPart(item.parts, DynamicPart::CornerFl) && !hasPart(item.parts, DynamicPart::WheelFl)) {
            ++model.binds.cornersFl;
        }
        if (hasPart(item.parts, DynamicPart::CornerFr) && !hasPart(item.parts, DynamicPart::WheelFr)) {
            ++model.binds.cornersFr;
        }
        if (hasPart(item.parts, DynamicPart::CornerRear) && !hasPart(item.parts, DynamicPart::WheelRl) &&
            !hasPart(item.parts, DynamicPart::WheelRr)) {
            ++model.binds.cornersRear;
        }
        if (hasPart(item.parts, DynamicPart::UpperWishboneFl) || hasPart(item.parts, DynamicPart::UpperWishboneFr) ||
            hasPart(item.parts, DynamicPart::LowerWishboneFl) || hasPart(item.parts, DynamicPart::LowerWishboneFr)) {
            ++model.binds.wishbones;
        }
        if (hasPart(item.parts, DynamicPart::PushrodFl) || hasPart(item.parts, DynamicPart::PushrodFr)) {
            ++model.binds.pushrods;
        }
        if (hasPart(item.parts, DynamicPart::TieRodFl) || hasPart(item.parts, DynamicPart::TieRodFr)) {
            ++model.binds.tieRods;
        }
        if (hasPart(item.parts, DynamicPart::ShockFl) || hasPart(item.parts, DynamicPart::ShockFr)) {
            ++model.binds.shocks;
        }
        if (hasPart(item.parts, DynamicPart::SteeringWheel) || hasPart(item.parts, DynamicPart::SteeringRack)) {
            ++model.binds.steering;
        }
        if (item.parts == DynamicPart::Body || item.parts == DynamicPart::None) {
            ++model.binds.body;
        }
    }
    auto collect = [&](DynamicPart want) {
        std::string names;
        for (const DrawItem& it : model.gpu.draws) {
            if (!hasPart(it.parts, want) || it.nodeName.empty()) {
                continue;
            }
            if (!names.empty()) {
                names += ",";
            }
            names += it.nodeName;
        }
        return names;
    };
    const std::string fl = collect(DynamicPart::WheelFl);
    const std::string fr = collect(DynamicPart::WheelFr);
    const std::string rear = collect(DynamicPart::WheelRl);
    if (model.rigid) {
        model.bindNote = "rigid display mesh — no articulated nodes";
        model.bindNodes = model.gpu.draws.empty() ? "" : model.gpu.draws.front().nodeName;
    } else {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "FL %d FR %d RL %d RR %d  corners %d/%d/%d  wb %d  rod %d  tie %d  shock %d  steer %d  body %d",
                      model.binds.wheelsFl, model.binds.wheelsFr, model.binds.wheelsRl, model.binds.wheelsRr,
                      model.binds.cornersFl, model.binds.cornersFr, model.binds.cornersRear, model.binds.wishbones,
                      model.binds.pushrods, model.binds.tieRods, model.binds.shocks, model.binds.steering,
                      model.binds.body);
        model.bindNote = buf;
        model.bindNodes.clear();
        if (!fl.empty()) {
            model.bindNodes += "FL ";
            model.bindNodes += fl;
        }
        if (!fr.empty()) {
            if (!model.bindNodes.empty()) {
                model.bindNodes += "  ";
            }
            model.bindNodes += "FR ";
            model.bindNodes += fr;
        }
        if (!rear.empty()) {
            if (!model.bindNodes.empty()) {
                model.bindNodes += "  ";
            }
            model.bindNodes += "REAR ";
            model.bindNodes += rear;
        }
    }
}

bool loadGltfMemory(Renderer& renderer, LoadedModel& out, const void* bytes, std::size_t size,
                    const char* name) {
    out = LoadedModel{};
    out.name = name != nullptr ? name : "memory";
    out.sourcePath = out.name;
    if (bytes == nullptr || size == 0) {
        out.error = "empty glTF buffer";
        return false;
    }
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err;
    std::string warn;
    const bool ok =
        loader.LoadBinaryFromMemory(&model, &err, &warn, static_cast<const unsigned char*>(bytes),
                                    static_cast<unsigned int>(size), "");
    if (!ok) {
        out.error = err.empty() ? "LoadBinaryFromMemory failed" : err;
        return false;
    }
    if (!warn.empty()) {
        std::fprintf(stderr, "[gl3d] gltf warn: %s\n", warn.c_str());
    }
    return ingestGltf(renderer, out, model);
}

void buildGround(Renderer& renderer, MeshGpu& out) {
    constexpr int kCells = 24;
    constexpr float kCell = 1.0f;
    constexpr float kY = 0.0f;
    // Center on new_dyn's wheelbase (AABB z mid ≈ -2.9) so the rear axle at z≈-5.8
    // sits on the checkerboard instead of the back edge of a z=0 grid.
    constexpr float kCenterZ = -2.9f;
    std::vector<Vertex> verts;
    std::vector<std::uint32_t> idx;
    verts.reserve(static_cast<std::size_t>(kCells * kCells * 4));
    idx.reserve(static_cast<std::size_t>(kCells * kCells * 6));
    const float half = static_cast<float>(kCells) * kCell * 0.5f;
    for (int cx = 0; cx < kCells; ++cx) {
        for (int cz = 0; cz < kCells; ++cz) {
            const float x0 = -half + static_cast<float>(cx) * kCell;
            const float z0 = kCenterZ - half + static_cast<float>(cz) * kCell;
            const bool light = ((cx + cz) & 1) != 0;
            const float r = light ? 0.20f : 0.12f;
            const float g = light ? 0.21f : 0.125f;
            const float b = light ? 0.23f : 0.135f;
            const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
            const Vec3 p[4] = {{x0, kY, z0},
                               {x0 + kCell, kY, z0},
                               {x0 + kCell, kY, z0 + kCell},
                               {x0, kY, z0 + kCell}};
            for (int k = 0; k < 4; ++k) {
                Vertex v{};
                v.pos[0] = p[k].x;
                v.pos[1] = p[k].y;
                v.pos[2] = p[k].z;
                v.nrm[1] = 1.0f;
                v.col[0] = r;
                v.col[1] = g;
                v.col[2] = b;
                v.col[3] = 1.0f;
                verts.push_back(v);
            }
            const std::uint32_t q[6] = {base, base + 2, base + 1, base, base + 3, base + 2};
            idx.insert(idx.end(), q, q + 6);
        }
    }
    renderer.uploadMesh(out, verts.data(), verts.size(), idx.data(), idx.size());
}

Mat4 chassisModel(float rollDeg, float pitchDeg) {
    // Photon's objectModelMatrix uses Vulkan Z-up pivot (0, 2.9, 0.72) after Rx(90).
    // Same point in Y-up glTF space: (0, 0.72, -2.9). Pitch about X, roll about Z.
    const Vec3 pivot{0.0f, 0.72f, -2.9f};
    return translate(pivot) * rotate(radians(pitchDeg), {1.0f, 0.0f, 0.0f}) *
           rotate(radians(rollDeg), {0.0f, 0.0f, 1.0f}) * translate(-pivot);
}

Mat4 itemModel(const DrawItem& item, const Kinematics& kin, float steerDeg, float steerWheelDeg, float spinFl,
               float spinFr, float spinRl, float spinRr, float suspFl, float suspFr, float suspRear) {
    const DynamicPart parts = item.parts;
    if (parts == DynamicPart::None) {
        return identity();
    }

    const auto rotateAt = [](Vec3 pivot, float angle, Vec3 axis) {
        return translate(pivot) * rotate(angle, axis) * translate(-pivot);
    };

    const auto wishboneTransform = [&](const Vec3& pivot, const Vec3& tip, float suspension) {
        const float angle = std::atan2((tip.y - pivot.y) + suspension, tip.x - pivot.x) -
                            std::atan2(tip.y - pivot.y, tip.x - pivot.x);
        return rotateAt(pivot, angle, {0.0f, 0.0f, 1.0f});
    };

    const auto wishboneMovedTip = [&](const Vec3& pivot, const Vec3& tip, float suspension) {
        return transformPoint(wishboneTransform(pivot, tip, suspension), tip);
    };

    const auto linkTransform = [&](Vec3 anchor, Vec3 tip, Vec3 movedAnchor, Vec3 movedTip) {
        const Vec3 from = tip - anchor;
        const Vec3 to = movedTip - movedAnchor;
        const float fromLength = length(from);
        const float toLength = length(to);
        if (fromLength < 1e-5f || toLength < 1e-5f) {
            return identity();
        }
        const Vec3 fromDirection = from * (1.0f / fromLength);
        const Vec3 toDirection = to * (1.0f / toLength);
        const float cosine = std::clamp(dot(fromDirection, toDirection), -1.0f, 1.0f);
        const float angle = std::acos(cosine);
        Vec3 axis = cross(fromDirection, toDirection);
        if (length(axis) < 1e-5f) {
            axis = {0.0f, 0.0f, 1.0f};
        }
        axis = normalize(axis);
        return translate(movedAnchor) * rotate(angle, axis) *
               scale({toLength / fromLength, toLength / fromLength, toLength / fromLength}) * translate(-anchor);
    };

    const auto rockerAngle = [](float side, float suspension) {
        return radians(suspension / 0.12f * 14.0f) * side;
    };

    const auto frontCornerTransform = [&](const SideKinematics& side, float suspension, float steering,
                                          float wheelRotation, bool spinWheel) {
        const Vec3 movedUpper = wishboneMovedTip(side.upperPivot, side.upperTip, suspension);
        const Vec3 movedLower = wishboneMovedTip(side.lowerPivot, side.lowerTip, suspension);
        const Vec3 baseMidpoint = (side.upperTip + side.lowerTip) * 0.5f;
        const Vec3 movedMidpoint = (movedUpper + movedLower) * 0.5f;
        const float camber = std::atan2(movedUpper.y - movedLower.y, movedUpper.x - movedLower.x) -
                             std::atan2(side.upperTip.y - side.lowerTip.y, side.upperTip.x - side.lowerTip.x);
        Mat4 result = translate(movedMidpoint) * rotate(camber, {0.0f, 0.0f, 1.0f}) * translate(-baseMidpoint);
        result = result * translate(side.wheelPivot) * rotate(radians(steering), {0.0f, 1.0f, 0.0f});
        if (spinWheel) {
            result = result * rotate(radians(wheelRotation), {1.0f, 0.0f, 0.0f});
        }
        return result * translate(-side.wheelPivot);
    };

    // Inboard follows the rocker; outboard follows the upright.
    const auto pushrodTransform = [&](const SideKinematics& side, float sideSign, float suspension) {
        const Vec3 movedAnchor =
            transformPoint(rotateAt(side.shockPivot, rockerAngle(sideSign, suspension), {0.0f, 0.0f, 1.0f}),
                           side.pushAnchor);
        const Vec3 movedTip =
            transformPoint(frontCornerTransform(side, suspension, 0.0f, 0.0f, false), side.pushTip);
        return linkTransform(side.pushAnchor, side.pushTip, movedAnchor, movedTip);
    };

    const auto tieRodTransform = [&](const SideKinematics& side, float suspension, float steering) {
        const float rackTravel = steering / 35.0f * 0.08f;
        const Vec3 movedAnchor = side.tieAnchor + Vec3{rackTravel, 0.0f, 0.0f};
        Vec3 movedTip = transformPoint(
            translate(side.wheelPivot) * rotate(radians(steering), {0.0f, 1.0f, 0.0f}) * translate(-side.wheelPivot),
            side.tieTip);
        movedTip.y += suspension;
        return linkTransform(side.tieAnchor, side.tieTip, movedAnchor, movedTip);
    };

    if (hasPart(parts, DynamicPart::UpperWishboneFl)) {
        return wishboneTransform(kin.fl.upperPivot, kin.fl.upperTip, suspFl);
    }
    if (hasPart(parts, DynamicPart::UpperWishboneFr)) {
        return wishboneTransform(kin.fr.upperPivot, kin.fr.upperTip, suspFr);
    }
    if (hasPart(parts, DynamicPart::LowerWishboneFl)) {
        return wishboneTransform(kin.fl.lowerPivot, kin.fl.lowerTip, suspFl);
    }
    if (hasPart(parts, DynamicPart::LowerWishboneFr)) {
        return wishboneTransform(kin.fr.lowerPivot, kin.fr.lowerTip, suspFr);
    }
    if (hasPart(parts, DynamicPart::PushrodFl)) {
        return pushrodTransform(kin.fl, -1.0f, suspFl);
    }
    if (hasPart(parts, DynamicPart::PushrodFr)) {
        return pushrodTransform(kin.fr, 1.0f, suspFr);
    }
    if (hasPart(parts, DynamicPart::TieRodFl)) {
        return tieRodTransform(kin.fl, suspFl, steerDeg);
    }
    if (hasPart(parts, DynamicPart::TieRodFr)) {
        return tieRodTransform(kin.fr, suspFr, steerDeg);
    }
    if (hasPart(parts, DynamicPart::CornerFl) || hasPart(parts, DynamicPart::WheelFl)) {
        return frontCornerTransform(kin.fl, suspFl, steerDeg, spinFl, hasPart(parts, DynamicPart::WheelFl));
    }
    if (hasPart(parts, DynamicPart::CornerFr) || hasPart(parts, DynamicPart::WheelFr)) {
        return frontCornerTransform(kin.fr, suspFr, steerDeg, spinFr, hasPart(parts, DynamicPart::WheelFr));
    }
    if (hasPart(parts, DynamicPart::SteeringRack)) {
        return translate({steerDeg / 35.0f * 0.08f, 0.0f, 0.0f});
    }
    if (hasPart(parts, DynamicPart::ShockFl) || hasPart(parts, DynamicPart::ShockFr)) {
        const bool left = hasPart(parts, DynamicPart::ShockFl);
        const float side = left ? -1.0f : 1.0f;
        const float suspension = left ? suspFl : suspFr;
        const Vec3 pivot = left ? kin.fl.shockPivot : kin.fr.shockPivot;
        const float angle = rockerAngle(side, suspension);
        return rotateAt(pivot, angle, {0.0f, 0.0f, 1.0f});
    }
    if (hasPart(parts, DynamicPart::CornerRear) || hasPart(parts, DynamicPart::WheelRl) ||
        hasPart(parts, DynamicPart::WheelRr)) {
        Vec3 pivot = kin.rearPivot;
        if (hasPart(parts, DynamicPart::WheelRl) || hasPart(parts, DynamicPart::WheelRr)) {
            pivot = {0.0f, kin.rearPivot.y, kin.rearPivot.z};
        } else if (std::fabs(item.pivot.y) > 0.2f) {
            pivot = item.pivot;
        }
        const bool hasRl = hasPart(parts, DynamicPart::WheelRl);
        const bool hasRr = hasPart(parts, DynamicPart::WheelRr);
        float spin = 0.0f;
        if (hasRl && hasRr) {
            spin = 0.5f * (spinRl + spinRr);
        } else if (hasRl) {
            spin = spinRl;
        } else if (hasRr) {
            spin = spinRr;
        }
        Mat4 transform = translate({0.0f, suspRear, 0.0f}) * translate(pivot);
        if (hasRl || hasRr) {
            transform = transform * rotate(radians(spin), {1.0f, 0.0f, 0.0f});
        }
        return transform * translate(-pivot);
    }
    if (hasPart(parts, DynamicPart::SteeringWheel)) {
        return rotateAt(kin.steerPivot, radians(steerWheelDeg), kin.steerAxis);
    }
    return identity();
}

}  // namespace photon::ui::gl3d
