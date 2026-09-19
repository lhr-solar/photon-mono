#ifndef __EMSCRIPTEN__

#include "photon_ui/gl_api.hpp"

#include <SDL.h>

#include <cstdio>

PFNGLGETBOOLEANVPROC glGetBooleanv = nullptr;
PFNGLCOLORMASKPROC glColorMask = nullptr;
PFNGLCULLFACEPROC glCullFace = nullptr;
PFNGLDEPTHMASKPROC glDepthMask = nullptr;
PFNGLDEPTHFUNCPROC glDepthFunc = nullptr;
PFNGLUNIFORM1FPROC glUniform1f = nullptr;
PFNGLUNIFORM3FPROC glUniform3f = nullptr;
PFNGLUNIFORM4FPROC glUniform4f = nullptr;
PFNGLUNIFORMMATRIX3FVPROC glUniformMatrix3fv = nullptr;
PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = nullptr;
PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer = nullptr;
PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers = nullptr;
PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers = nullptr;
PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer = nullptr;
PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage = nullptr;

namespace {

template <typename T>
T load(const char* name) {
    return reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
}

}  // namespace

namespace {
bool gGlLive = false;
}

void photonGlMarkContextLost() { gGlLive = false; }

bool photonGlContextLive() { return gGlLive; }

bool photonGlExtrasReady() {
    return gGlLive && glGetBooleanv && glColorMask && glCullFace && glDepthMask && glDepthFunc &&
           glUniform1f && glUniform3f && glUniform4f && glUniformMatrix3fv && glGenFramebuffers &&
           glDeleteFramebuffers && glBindFramebuffer && glCheckFramebufferStatus &&
           glFramebufferTexture2D && glFramebufferRenderbuffer && glGenRenderbuffers &&
           glDeleteRenderbuffers && glBindRenderbuffer && glRenderbufferStorage;
}

bool photonGlLoadExtras() {
    glGetBooleanv = load<PFNGLGETBOOLEANVPROC>("glGetBooleanv");
    glColorMask = load<PFNGLCOLORMASKPROC>("glColorMask");
    glCullFace = load<PFNGLCULLFACEPROC>("glCullFace");
    glDepthMask = load<PFNGLDEPTHMASKPROC>("glDepthMask");
    glDepthFunc = load<PFNGLDEPTHFUNCPROC>("glDepthFunc");
    glUniform1f = load<PFNGLUNIFORM1FPROC>("glUniform1f");
    glUniform3f = load<PFNGLUNIFORM3FPROC>("glUniform3f");
    glUniform4f = load<PFNGLUNIFORM4FPROC>("glUniform4f");
    glUniformMatrix3fv = load<PFNGLUNIFORMMATRIX3FVPROC>("glUniformMatrix3fv");
    glGenFramebuffers = load<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers");
    glDeleteFramebuffers = load<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers");
    glBindFramebuffer = load<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer");
    glCheckFramebufferStatus = load<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus");
    glFramebufferTexture2D = load<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D");
    glFramebufferRenderbuffer = load<PFNGLFRAMEBUFFERRENDERBUFFERPROC>("glFramebufferRenderbuffer");
    glGenRenderbuffers = load<PFNGLGENRENDERBUFFERSPROC>("glGenRenderbuffers");
    glDeleteRenderbuffers = load<PFNGLDELETERENDERBUFFERSPROC>("glDeleteRenderbuffers");
    glBindRenderbuffer = load<PFNGLBINDRENDERBUFFERPROC>("glBindRenderbuffer");
    glRenderbufferStorage = load<PFNGLRENDERBUFFERSTORAGEPROC>("glRenderbufferStorage");

    const bool ok = glGetBooleanv && glColorMask && glCullFace && glDepthMask && glDepthFunc && glUniform1f &&
                    glUniform3f && glUniform4f && glUniformMatrix3fv && glGenFramebuffers && glDeleteFramebuffers &&
                    glBindFramebuffer && glCheckFramebufferStatus && glFramebufferTexture2D &&
                    glFramebufferRenderbuffer && glGenRenderbuffers && glDeleteRenderbuffers && glBindRenderbuffer &&
                    glRenderbufferStorage;
    if (!ok) {
        std::fprintf(stderr, "photon-ui: failed to load extra GL entry points\n");
        gGlLive = false;
        return false;
    }
    gGlLive = true;
    return true;
}

#endif
