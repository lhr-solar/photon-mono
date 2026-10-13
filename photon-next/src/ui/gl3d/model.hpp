#pragma once

#include "ui/gl3d/renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace photon::ui::gl3d {

struct BindStats {
    int wheelsFl = 0;
    int wheelsFr = 0;
    int wheelsRl = 0;
    int wheelsRr = 0;
    int cornersFl = 0;
    int cornersFr = 0;
    int cornersRear = 0;
    int wishbones = 0;
    int pushrods = 0;
    int tieRods = 0;
    int shocks = 0;
    int steering = 0;
    int body = 0;
};

// Rest-pose attach points measured from the mesh at load (inboard centroid +
// outboard-most vertex per link). Fall back to new_dyn.glb measured defaults.
struct SideKinematics {
    Vec3 upperPivot{};
    Vec3 upperTip{};
    Vec3 lowerPivot{};
    Vec3 lowerTip{};
    Vec3 pushAnchor{};
    Vec3 pushTip{};
    Vec3 tieAnchor{};
    Vec3 tieTip{};
    Vec3 shockPivot{};
    Vec3 wheelPivot{};
};

struct Kinematics {
    SideKinematics fl{};
    SideKinematics fr{};
    Vec3 steerPivot{0.00065f, 1.67644f, -0.94506f};
    Vec3 steerAxis{0.00018f, 0.42394f, -0.90569f};
    Vec3 rearPivot{0.0f, 0.708f, -5.790f};
};

struct LoadedModel {
    ModelGpu gpu{};
    std::string name;
    std::string sourcePath;
    std::string error;
    std::string bindNote;
    std::string bindNodes;
    BindStats binds{};
    Kinematics kin{};
    Vec3 aabbMin{0.0f, 0.0f, 0.0f};
    Vec3 aabbMax{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    std::uint32_t triangleCount = 0;
    bool haveAabb = false;
    bool fromGltf = false;
    bool rigid = false;
};

bool loadGltfMemory(Renderer& renderer, LoadedModel& out, const void* bytes, std::size_t size,
                    const char* name);
void buildGround(Renderer& renderer, MeshGpu& out);
void summarizeBinds(LoadedModel& model);

// Chassis attitude around the measured new_dyn mid-wheelbase pivot (Y-up).
// Applied as a parent of every item matrix, matching Photon's objectModelMatrix.
Mat4 chassisModel(float rollDeg, float pitchDeg);

// Per-part IK. Attach points come from LoadedModel.kin (measured at load).
Mat4 itemModel(const DrawItem& item, const Kinematics& kin, float steerDeg, float steerWheelDeg, float spinFl,
               float spinFr, float spinRl, float spinRr, float suspFl, float suspFr, float suspRear);

}  // namespace photon::ui::gl3d
