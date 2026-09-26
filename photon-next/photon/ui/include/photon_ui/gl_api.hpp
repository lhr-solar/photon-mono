#pragma once

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else

// Desktop GL comes from ImGui's embedded loader plus a few extras the 3D
// renderer needs (FBO / uniforms) that imgui_impl_opengl3 does not touch.
#include "imgui_impl_opengl3_loader.h"

#ifndef GL_ZERO
#define GL_ZERO 0
#endif
#ifndef GL_ONE
#define GL_ONE 1
#endif
#ifndef GL_LESS
#define GL_LESS 0x0201
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_DEPTH_BUFFER_BIT
#define GL_DEPTH_BUFFER_BIT 0x00000100
#endif
#ifndef GL_DEPTH_WRITEMASK
#define GL_DEPTH_WRITEMASK 0x0B72
#endif
#ifndef GL_COLOR_WRITEMASK
#define GL_COLOR_WRITEMASK 0x0C23
#endif
#ifndef GL_CULL_FACE_MODE
#define GL_CULL_FACE_MODE 0x0B45
#endif
#ifndef GL_DEPTH_FUNC
#define GL_DEPTH_FUNC 0x0B74
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16 0x81A5
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

using PFNGLGETBOOLEANVPROC = void (*)(GLenum, GLboolean*);
using PFNGLCOLORMASKPROC = void (*)(GLboolean, GLboolean, GLboolean, GLboolean);
using PFNGLCULLFACEPROC = void (*)(GLenum);
using PFNGLDEPTHMASKPROC = void (*)(GLboolean);
using PFNGLDEPTHFUNCPROC = void (*)(GLenum);
using PFNGLUNIFORM1FPROC = void (*)(GLint, GLfloat);
using PFNGLUNIFORM3FPROC = void (*)(GLint, GLfloat, GLfloat, GLfloat);
using PFNGLUNIFORM4FPROC = void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using PFNGLUNIFORMMATRIX3FVPROC = void (*)(GLint, GLsizei, GLboolean, const GLfloat*);
using PFNGLGENFRAMEBUFFERSPROC = void (*)(GLsizei, GLuint*);
using PFNGLDELETEFRAMEBUFFERSPROC = void (*)(GLsizei, const GLuint*);
using PFNGLBINDFRAMEBUFFERPROC = void (*)(GLenum, GLuint);
using PFNGLCHECKFRAMEBUFFERSTATUSPROC = GLenum (*)(GLenum);
using PFNGLFRAMEBUFFERTEXTURE2DPROC = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFNGLFRAMEBUFFERRENDERBUFFERPROC = void (*)(GLenum, GLenum, GLenum, GLuint);
using PFNGLGENRENDERBUFFERSPROC = void (*)(GLsizei, GLuint*);
using PFNGLDELETERENDERBUFFERSPROC = void (*)(GLsizei, const GLuint*);
using PFNGLBINDRENDERBUFFERPROC = void (*)(GLenum, GLuint);
using PFNGLRENDERBUFFERSTORAGEPROC = void (*)(GLenum, GLenum, GLsizei, GLsizei);

extern PFNGLGETBOOLEANVPROC glGetBooleanv;
extern PFNGLCOLORMASKPROC glColorMask;
extern PFNGLCULLFACEPROC glCullFace;
extern PFNGLDEPTHMASKPROC glDepthMask;
extern PFNGLDEPTHFUNCPROC glDepthFunc;
extern PFNGLUNIFORM1FPROC glUniform1f;
extern PFNGLUNIFORM3FPROC glUniform3f;
extern PFNGLUNIFORM4FPROC glUniform4f;
extern PFNGLUNIFORMMATRIX3FVPROC glUniformMatrix3fv;
extern PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
extern PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers;
extern PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers;
extern PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer;
extern PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage;

bool photonGlLoadExtras();
void photonGlMarkContextLost();
bool photonGlContextLive();
bool photonGlExtrasReady();

#endif

#ifdef __EMSCRIPTEN__
inline void photonGlMarkContextLost() {}
inline bool photonGlContextLive() { return true; }
inline bool photonGlExtrasReady() { return true; }
#endif
