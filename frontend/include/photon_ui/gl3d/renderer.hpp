#pragma once

#include "photon_ui/gl3d/math.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace photon::ui::gl3d {

struct Vertex {
    float pos[3]{};
    float nrm[3]{};
    float uv[2]{};
    float col[4]{1.0f, 1.0f, 1.0f, 1.0f};
};

enum class DynamicPart : std::uint32_t {
    None = 0,
    Body = 1u << 0,
    WheelFl = 1u << 1,
    WheelFr = 1u << 2,
    WheelRl = 1u << 3,
    WheelRr = 1u << 4,
    CornerFl = 1u << 5,
    CornerFr = 1u << 6,
    CornerRear = 1u << 7,
    UpperWishboneFl = 1u << 8,
    UpperWishboneFr = 1u << 9,
    LowerWishboneFl = 1u << 10,
    LowerWishboneFr = 1u << 11,
    PushrodFl = 1u << 12,
    PushrodFr = 1u << 13,
    TieRodFl = 1u << 14,
    TieRodFr = 1u << 15,
    ShockFl = 1u << 16,
    ShockFr = 1u << 17,
    SteeringRack = 1u << 18,
    SteeringWheel = 1u << 19,
};

inline constexpr DynamicPart operator|(DynamicPart a, DynamicPart b) {
    return static_cast<DynamicPart>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline constexpr bool hasPart(DynamicPart have, DynamicPart want) {
    return (static_cast<std::uint32_t>(have) & static_cast<std::uint32_t>(want)) != 0;
}

struct MaterialGpu {
    Vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    unsigned texture = 0;
    bool hasTexture = false;
    bool doubleSided = false;
};

struct DrawItem {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t material = 0;
    DynamicPart parts = DynamicPart::None;
    Vec3 pivot{};
    Vec3 bmin{};
    Vec3 bmax{};
    Vec3 anchor{};
    Vec3 tip{};
    std::string nodeName;
};

struct MeshGpu {
    unsigned vao = 0;
    unsigned vbo = 0;
    unsigned ebo = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t vertexCount = 0;
};

struct ModelGpu {
    MeshGpu mesh{};
    std::vector<DrawItem> draws;
    std::vector<MaterialGpu> materials;
    std::vector<unsigned> textures;
    bool loaded = false;
};

struct Lights {
    Vec3 keyDir{0.35f, 0.82f, 0.45f};
    Vec3 keyColor{0.95f, 0.93f, 0.88f};
    Vec3 fillDir{-0.55f, 0.40f, -0.35f};
    Vec3 fillColor{0.80f, 0.55f, 0.38f};
    Vec3 ambient{0.52f, 0.54f, 0.60f};
    Vec3 clearColor{0.12f, 0.12f, 0.13f};
    float exposure = 2.0f;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool ensureProgram();
    void uploadMesh(MeshGpu& mesh, const Vertex* verts, std::size_t vcount, const std::uint32_t* idx,
                    std::size_t icount);
    unsigned uploadTextureRgba(int w, int h, const unsigned char* rgba, bool srgb);
    unsigned whiteTexture();
    void destroyMesh(MeshGpu& mesh);
    void destroyModel(ModelGpu& model);

    // Render into an offscreen color texture (ImGui::Image). Restores GL state
    // ImGui's GLES3 backend needs on the default framebuffer.
    unsigned render(int width, int height, const Mat4& view, const Mat4& proj, const ModelGpu& model,
                    const Mat4* itemModels, const MeshGpu* extra, const Mat4* extraModel,
                    const ModelGpu* track, const Mat4* trackModel, const Lights& lights);

    int lastDrawCalls() const { return lastDraws_; }

private:
    unsigned program_ = 0;
    unsigned fbo_ = 0;
    unsigned color_ = 0;
    unsigned depth_ = 0;
    unsigned white_ = 0;
    int fboW_ = 0;
    int fboH_ = 0;
    int lastDraws_ = 0;
    int uMvp_ = -1;
    int uModel_ = -1;
    int uNormal_ = -1;
    int uLightDir_ = -1;
    int uAmbient_ = -1;
    int uLightColor_ = -1;
    int uFillDir_ = -1;
    int uFillColor_ = -1;
    int uExposure_ = -1;
    int uBaseColor_ = -1;
    int uTex_ = -1;
    int uHasTex_ = -1;

    bool ensureTarget(int width, int height);
    void destroyTarget();
    void destroyProgram();
};

}  // namespace photon::ui::gl3d
