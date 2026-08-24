#include "photon_ui/gl3d/renderer.hpp"

#include <GLES3/gl3.h>

#include <cstdio>
#include <cstring>

namespace photon::ui::gl3d {
namespace {

const char* kVert = R"(#version 300 es
precision highp float;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aCol;
uniform mat4 uMvp;
uniform mat4 uModel;
uniform mat3 uNormal;
out vec3 vNrm;
out vec2 vUv;
out vec4 vCol;
void main() {
    vNrm = normalize(uNormal * aNrm);
    vUv = aUv;
    vCol = aCol;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char* kFrag = R"(#version 300 es
precision highp float;
in vec3 vNrm;
in vec2 vUv;
in vec4 vCol;
out vec4 oColor;
uniform vec3 uLightDir;
uniform vec3 uAmbient;
uniform vec3 uLightColor;
uniform vec3 uFillDir;
uniform vec3 uFillColor;
uniform float uExposure;
uniform vec4 uBaseColor;
uniform sampler2D uTex;
uniform int uHasTex;
void main() {
    vec4 base = uBaseColor * vCol;
    if (uHasTex == 1) {
        base *= texture(uTex, vUv);
    }
    vec3 n = normalize(vNrm);
    float ndl = max(dot(n, normalize(uLightDir)), 0.0);
    float ndlFill = max(dot(n, normalize(uFillDir)), 0.0);
    vec3 lin = base.rgb * (uAmbient + uLightColor * ndl + uFillColor * ndlFill);
    lin *= max(uExposure, 0.0);
    oColor = vec4(pow(max(lin, vec3(0.0)), vec3(1.0 / 2.2)), base.a);
}
)";

struct GlState {
    GLint fbo = 0;
    GLint viewport[4]{};
    GLint program = 0;
    GLint vao = 0;
    GLint arrayBuf = 0;
    GLint elemBuf = 0;
    GLint activeTex = 0;
    GLint tex2d = 0;
    GLint unpackAlign = 4;
    GLboolean depthTest = GL_FALSE;
    GLboolean cullFace = GL_FALSE;
    GLboolean blend = GL_FALSE;
    GLboolean scissor = GL_FALSE;
    GLboolean depthMask = GL_TRUE;
    GLboolean colorMask[4]{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint depthFunc = GL_LESS;
    GLint cullMode = GL_BACK;
    GLint blendSrcRgb = GL_ONE;
    GLint blendDstRgb = GL_ZERO;
    GLint blendSrcA = GL_ONE;
    GLint blendDstA = GL_ZERO;
};

void saveState(GlState& s) {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.elemBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex2d);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s.unpackAlign);
    s.depthTest = glIsEnabled(GL_DEPTH_TEST);
    s.cullFace = glIsEnabled(GL_CULL_FACE);
    s.blend = glIsEnabled(GL_BLEND);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    glGetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
    glGetIntegerv(GL_CULL_FACE_MODE, &s.cullMode);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendDstA);
}

void restoreState(const GlState& s) {
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(s.fbo));
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glUseProgram(static_cast<GLuint>(s.program));
    glBindVertexArray(static_cast<GLuint>(s.vao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(s.arrayBuf));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(s.elemBuf));
    glActiveTexture(static_cast<GLenum>(s.activeTex));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s.tex2d));
    glPixelStorei(GL_UNPACK_ALIGNMENT, s.unpackAlign);
    if (s.depthTest != 0) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (s.cullFace != 0) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
    if (s.blend != 0) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    if (s.scissor != 0) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glDepthMask(s.depthMask);
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    glDepthFunc(static_cast<GLenum>(s.depthFunc));
    glCullFace(static_cast<GLenum>(s.cullMode));
    glBlendFuncSeparate(static_cast<GLenum>(s.blendSrcRgb), static_cast<GLenum>(s.blendDstRgb),
                        static_cast<GLenum>(s.blendSrcA), static_cast<GLenum>(s.blendDstA));
}

GLuint compile(GLenum type, const char* src) {
    const GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        char log[256];
        glGetShaderInfoLog(sh, static_cast<GLsizei>(sizeof(log)), nullptr, log);
        std::fprintf(stderr, "[gl3d] shader compile: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

void bindAttribs() {
    const GLsizei stride = static_cast<GLsizei>(sizeof(Vertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, nrm)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, uv)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, col)));
}

}  // namespace

Renderer::~Renderer() {
    destroyTarget();
    destroyProgram();
    if (white_ != 0) {
        glDeleteTextures(1, &white_);
        white_ = 0;
    }
}

void Renderer::destroyMesh(MeshGpu& mesh) {
    if (mesh.vao != 0) {
        glDeleteVertexArrays(1, &mesh.vao);
        mesh.vao = 0;
    }
    if (mesh.vbo != 0) {
        glDeleteBuffers(1, &mesh.vbo);
        mesh.vbo = 0;
    }
    if (mesh.ebo != 0) {
        glDeleteBuffers(1, &mesh.ebo);
        mesh.ebo = 0;
    }
    mesh.indexCount = 0;
    mesh.vertexCount = 0;
}

void Renderer::destroyModel(ModelGpu& model) {
    destroyMesh(model.mesh);
    for (unsigned tex : model.textures) {
        if (tex != 0) {
            glDeleteTextures(1, &tex);
        }
    }
    model.textures.clear();
    model.materials.clear();
    model.draws.clear();
    model.loaded = false;
}

void Renderer::destroyProgram() {
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void Renderer::destroyTarget() {
    if (color_ != 0) {
        glDeleteTextures(1, &color_);
        color_ = 0;
    }
    if (depth_ != 0) {
        glDeleteRenderbuffers(1, &depth_);
        depth_ = 0;
    }
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    fboW_ = 0;
    fboH_ = 0;
}

bool Renderer::ensureProgram() {
    if (program_ != 0) {
        return true;
    }
    const GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (vs == 0 || fs == 0) {
        if (vs != 0) {
            glDeleteShader(vs);
        }
        if (fs != 0) {
            glDeleteShader(fs);
        }
        return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (ok == 0) {
        char log[256];
        glGetProgramInfoLog(program_, static_cast<GLsizei>(sizeof(log)), nullptr, log);
        std::fprintf(stderr, "[gl3d] program link: %s\n", log);
        destroyProgram();
        return false;
    }
    uMvp_ = glGetUniformLocation(program_, "uMvp");
    uModel_ = glGetUniformLocation(program_, "uModel");
    uNormal_ = glGetUniformLocation(program_, "uNormal");
    uLightDir_ = glGetUniformLocation(program_, "uLightDir");
    uAmbient_ = glGetUniformLocation(program_, "uAmbient");
    uLightColor_ = glGetUniformLocation(program_, "uLightColor");
    uFillDir_ = glGetUniformLocation(program_, "uFillDir");
    uFillColor_ = glGetUniformLocation(program_, "uFillColor");
    uExposure_ = glGetUniformLocation(program_, "uExposure");
    uBaseColor_ = glGetUniformLocation(program_, "uBaseColor");
    uTex_ = glGetUniformLocation(program_, "uTex");
    uHasTex_ = glGetUniformLocation(program_, "uHasTex");
    return true;
}

bool Renderer::ensureTarget(int width, int height) {
    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }
    if (fbo_ != 0 && fboW_ == width && fboH_ == height) {
        return true;
    }
    destroyTarget();
    glGenTextures(1, &color_);
    glBindTexture(GL_TEXTURE_2D, color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[gl3d] framebuffer incomplete 0x%x\n", static_cast<unsigned>(status));
        destroyTarget();
        return false;
    }
    fboW_ = width;
    fboH_ = height;
    return true;
}

void Renderer::uploadMesh(MeshGpu& mesh, const Vertex* verts, std::size_t vcount, const std::uint32_t* idx,
                          std::size_t icount) {
    if (mesh.vao == 0) {
        glGenVertexArrays(1, &mesh.vao);
        glGenBuffers(1, &mesh.vbo);
        glGenBuffers(1, &mesh.ebo);
    }
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vcount * sizeof(Vertex)), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(icount * sizeof(std::uint32_t)), idx,
                 GL_STATIC_DRAW);
    bindAttribs();
    glBindVertexArray(0);
    mesh.vertexCount = static_cast<std::uint32_t>(vcount);
    mesh.indexCount = static_cast<std::uint32_t>(icount);
}

unsigned Renderer::uploadTextureRgba(int w, int h, const unsigned char* rgba, bool srgb) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const GLint internal = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return tex;
}

unsigned Renderer::whiteTexture() {
    if (white_ != 0) {
        return white_;
    }
    const unsigned char px[4] = {255, 255, 255, 255};
    white_ = uploadTextureRgba(1, 1, px, false);
    return white_;
}

unsigned Renderer::render(int width, int height, const Mat4& view, const Mat4& proj, const ModelGpu& model,
                          const Mat4* itemModels, const MeshGpu* extra, const Mat4* extraModel,
                          const ModelGpu* track, const Mat4* trackModel, const Lights& lights) {
    lastDraws_ = 0;
    if (!ensureProgram()) {
        return 0;
    }
    GlState prev{};
    saveState(prev);
    if (!ensureTarget(width, height)) {
        restoreState(prev);
        return 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, fboW_, fboH_);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(lights.clearColor.x, lights.clearColor.y, lights.clearColor.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(program_);
    const Mat4 vp = proj * view;
    glUniform3f(uLightDir_, lights.keyDir.x, lights.keyDir.y, lights.keyDir.z);
    glUniform3f(uAmbient_, lights.ambient.x, lights.ambient.y, lights.ambient.z);
    glUniform3f(uLightColor_, lights.keyColor.x, lights.keyColor.y, lights.keyColor.z);
    glUniform3f(uFillDir_, lights.fillDir.x, lights.fillDir.y, lights.fillDir.z);
    glUniform3f(uFillColor_, lights.fillColor.x, lights.fillColor.y, lights.fillColor.z);
    glUniform1f(uExposure_, lights.exposure);
    glUniform1i(uTex_, 0);

    const auto drawMesh = [&](const MeshGpu& mesh, const DrawItem* items, std::size_t nItems,
                              const Mat4* models, const std::vector<MaterialGpu>* mats) {
        if (mesh.vao == 0 || mesh.indexCount == 0) {
            return;
        }
        glBindVertexArray(mesh.vao);
        if (items == nullptr || nItems == 0) {
            Mat4 modelM = extraModel != nullptr ? *extraModel : identity();
            const Mat4 mvp = vp * modelM;
            float nrm[9]{};
            normalMatrix(modelM, nrm);
            glUniformMatrix4fv(uMvp_, 1, GL_FALSE, mvp.m);
            glUniformMatrix4fv(uModel_, 1, GL_FALSE, modelM.m);
            glUniformMatrix3fv(uNormal_, 1, GL_FALSE, nrm);
            glUniform4f(uBaseColor_, 1.0f, 1.0f, 1.0f, 1.0f);
            glUniform1i(uHasTex_, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, whiteTexture());
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
            ++lastDraws_;
            return;
        }
        for (std::size_t i = 0; i < nItems; ++i) {
            const DrawItem& item = items[i];
            const Mat4 modelM = models != nullptr ? models[i] : identity();
            const Mat4 mvp = vp * modelM;
            float nrm[9]{};
            normalMatrix(modelM, nrm);
            glUniformMatrix4fv(uMvp_, 1, GL_FALSE, mvp.m);
            glUniformMatrix4fv(uModel_, 1, GL_FALSE, modelM.m);
            glUniformMatrix3fv(uNormal_, 1, GL_FALSE, nrm);
            Vec4 base{1.0f, 1.0f, 1.0f, 1.0f};
            unsigned tex = whiteTexture();
            bool hasTex = false;
            if (mats != nullptr && item.material < mats->size()) {
                const MaterialGpu& mat = (*mats)[item.material];
                base = mat.baseColor;
                if (mat.hasTexture && mat.texture != 0) {
                    tex = mat.texture;
                    hasTex = true;
                }
            }
            if (mats != nullptr && item.material < mats->size() && (*mats)[item.material].doubleSided) {
                glDisable(GL_CULL_FACE);
            } else {
                glEnable(GL_CULL_FACE);
            }
            glUniform4f(uBaseColor_, base.x, base.y, base.z, base.w);
            glUniform1i(uHasTex_, hasTex ? 1 : 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            const GLvoid* off = reinterpret_cast<const GLvoid*>(
                static_cast<std::uintptr_t>(item.firstIndex) * sizeof(std::uint32_t));
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(item.indexCount), GL_UNSIGNED_INT, off);
            ++lastDraws_;
        }
    };

    if (track != nullptr && track->loaded) {
        glDisable(GL_CULL_FACE);
        drawMesh(track->mesh, track->draws.data(), track->draws.size(), trackModel, &track->materials);
        glEnable(GL_CULL_FACE);
    }
    if (extra != nullptr) {
        drawMesh(*extra, nullptr, 0, nullptr, nullptr);
    }
    if (model.loaded) {
        drawMesh(model.mesh, model.draws.data(), model.draws.size(), itemModels, &model.materials);
    }

    restoreState(prev);
    return color_;
}

}  // namespace photon::ui::gl3d
