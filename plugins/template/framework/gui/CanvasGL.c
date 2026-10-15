// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The OpenGL half of the renderer seam (gui/Canvas.h): the single translation
// unit that instantiates NanoVG's OpenGL backend.
//
// GL2 rather than GL3: everything this panel draws is ordinary filled and
// stroked paths, so the VAO-based GL3 path buys nothing measurable, while GL2
// links directly against the system GL on both Linux (Mesa exports the 2.0
// entry points) and macOS (the OpenGL framework does the same).
//
// Windows is the exception, and the reason for the bulk of this file:
// opengl32.dll has exported nothing past GL 1.1 since 1996, and the Windows SDK
// ships no glext.h, so both the entry points and the enums have to be declared
// here and resolved through wglGetProcAddress. It is mechanical rather than
// subtle, and it keeps the mess confined to one place.

#if defined(_WIN32)

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <stddef.h>
#  include <GL/gl.h>

typedef char      GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#  ifndef GL_ARRAY_BUFFER
#    define GL_ARRAY_BUFFER      0x8892
#    define GL_STREAM_DRAW       0x88E0
#    define GL_FRAGMENT_SHADER   0x8B30
#    define GL_VERTEX_SHADER     0x8B31
#    define GL_COMPILE_STATUS    0x8B81
#    define GL_LINK_STATUS       0x8B82
#    define GL_TEXTURE0          0x84C0
#    define GL_UNPACK_SKIP_PIXELS 0x0CF4
#    define GL_UNPACK_SKIP_ROWS  0x0CF3
#  endif
// GL 1.2 to 1.4 enums NanoVG's GL2 backend uses, also absent from the SDK.
#  ifndef GL_CLAMP_TO_EDGE
#    define GL_CLAMP_TO_EDGE     0x812F
#  endif
#  ifndef GL_GENERATE_MIPMAP
#    define GL_GENERATE_MIPMAP   0x8191
#  endif
#  ifndef GL_INCR_WRAP
#    define GL_INCR_WRAP         0x8507
#    define GL_DECR_WRAP         0x8508
#  endif

// name, return type, parameter list -- one row per entry point NanoVG's GL2
// backend calls above GL 1.1.
#  define SUPPLE_GL_PROCS                                                     \
    X(glActiveTexture, void, (GLenum))                                         \
    X(glAttachShader, void, (GLuint, GLuint))                                  \
    X(glBindAttribLocation, void, (GLuint, GLuint, const GLchar *))            \
    X(glBindBuffer, void, (GLenum, GLuint))                                    \
    X(glBlendFuncSeparate, void, (GLenum, GLenum, GLenum, GLenum))             \
    X(glBufferData, void, (GLenum, GLsizeiptr, const void *, GLenum))          \
    X(glCompileShader, void, (GLuint))                                         \
    X(glCreateProgram, GLuint, (void))                                         \
    X(glCreateShader, GLuint, (GLenum))                                        \
    X(glDeleteBuffers, void, (GLsizei, const GLuint *))                        \
    X(glDeleteProgram, void, (GLuint))                                         \
    X(glDeleteShader, void, (GLuint))                                          \
    X(glDisableVertexAttribArray, void, (GLuint))                              \
    X(glEnableVertexAttribArray, void, (GLuint))                               \
    X(glGenBuffers, void, (GLsizei, GLuint *))                                 \
    X(glGenerateMipmap, void, (GLenum))                                        \
    X(glGetProgramInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *))       \
    X(glGetProgramiv, void, (GLuint, GLenum, GLint *))                         \
    X(glGetShaderInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *))        \
    X(glGetShaderiv, void, (GLuint, GLenum, GLint *))                          \
    X(glGetUniformLocation, GLint, (GLuint, const GLchar *))                   \
    X(glLinkProgram, void, (GLuint))                                           \
    X(glShaderSource, void, (GLuint, GLsizei, const GLchar *const *,           \
                             const GLint *))                                   \
    X(glStencilOpSeparate, void, (GLenum, GLenum, GLenum, GLenum))             \
    X(glUniform1i, void, (GLint, GLint))                                       \
    X(glUniform2fv, void, (GLint, GLsizei, const GLfloat *))                   \
    X(glUniform4fv, void, (GLint, GLsizei, const GLfloat *))                   \
    X(glUseProgram, void, (GLuint))                                            \
    X(glVertexAttribPointer, void, (GLuint, GLint, GLenum, GLboolean, GLsizei, \
                                    const void *))

#  define X(name, ret, params) typedef ret(APIENTRY *supple_pfn_##name) params;
SUPPLE_GL_PROCS
#  undef X

#  define X(name, ret, params) static supple_pfn_##name supple_##name;
SUPPLE_GL_PROCS
#  undef X

// wglGetProcAddress does not signal failure with NULL alone: the documented
// failure values are 0, 1, 2, 3 and -1, and drivers have historically returned
// the small integers. Treating one of those as a function pointer crashes on
// the first call rather than failing here, where the caller already has a
// clean path for a context it cannot use.
static PROC suppleResolveGL(const char* name)
{
    PROC p = wglGetProcAddress(name);
    if (p == 0 || p == (PROC)1 || p == (PROC)2 || p == (PROC)3 ||
        p == (PROC)-1) {
        // The pre-1.5 entry points can live in opengl32.dll instead, where
        // wglGetProcAddress is not required to find them.
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        p = gl ? GetProcAddress(gl, name) : 0;
    }
    return p;
}

// Through void (*)(void), the conventional generic function pointer: GCC's
// -Wcast-function-type accepts a cast from it, where a cast from PROC itself
// warns for every entry point.
int canvasLoad(void)
{
#  define X(name, ret, params)                                                 \
    supple_##name = (supple_pfn_##name)(void (*)(void))suppleResolveGL(#name); \
    if (!supple_##name) return 0;
    SUPPLE_GL_PROCS
#  undef X
    return 1;
}

// Redirect the names onto the resolved pointers for everything below.
#  define glActiveTexture            supple_glActiveTexture
#  define glAttachShader             supple_glAttachShader
#  define glBindAttribLocation       supple_glBindAttribLocation
#  define glBindBuffer               supple_glBindBuffer
#  define glBlendFuncSeparate        supple_glBlendFuncSeparate
#  define glBufferData               supple_glBufferData
#  define glCompileShader            supple_glCompileShader
#  define glCreateProgram            supple_glCreateProgram
#  define glCreateShader             supple_glCreateShader
#  define glDeleteBuffers            supple_glDeleteBuffers
#  define glDeleteProgram            supple_glDeleteProgram
#  define glDeleteShader             supple_glDeleteShader
#  define glDisableVertexAttribArray supple_glDisableVertexAttribArray
#  define glEnableVertexAttribArray  supple_glEnableVertexAttribArray
#  define glGenBuffers               supple_glGenBuffers
#  define glGenerateMipmap           supple_glGenerateMipmap
#  define glGetProgramInfoLog        supple_glGetProgramInfoLog
#  define glGetProgramiv             supple_glGetProgramiv
#  define glGetShaderInfoLog         supple_glGetShaderInfoLog
#  define glGetShaderiv              supple_glGetShaderiv
#  define glGetUniformLocation       supple_glGetUniformLocation
#  define glLinkProgram              supple_glLinkProgram
#  define glShaderSource             supple_glShaderSource
#  define glStencilOpSeparate        supple_glStencilOpSeparate
#  define glUniform1i                supple_glUniform1i
#  define glUniform2fv               supple_glUniform2fv
#  define glUniform4fv               supple_glUniform4fv
#  define glUseProgram               supple_glUseProgram
#  define glVertexAttribPointer      supple_glVertexAttribPointer

#elif defined(__APPLE__)

#  ifndef GL_SILENCE_DEPRECATION  // also set on the pugl target
#    define GL_SILENCE_DEPRECATION
#  endif
#  include <OpenGL/gl.h>

int canvasLoad(void) { return 1; }

#else

// glext.h declares the post-1.1 entry points only when GL_GLEXT_PROTOTYPES
// is set; without it the header is typedefs alone and every call NanoVG
// makes above GL 1.1 is an implicit declaration. Mesa's libGL exports the
// symbols, so the prototypes are all that is missing.
#  ifndef GL_GLEXT_PROTOTYPES
#    define GL_GLEXT_PROTOTYPES 1
#  endif
#  include <GL/gl.h>
#  include <GL/glext.h>

int canvasLoad(void) { return 1; }

#endif

#define NANOVG_GL2_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_gl.h"

#include "gui/Canvas.h"

// Context creation lives behind the same seam as everything else here, so that
// Window.cpp names no renderer at all. The Metal backend's version of this
// takes the CAMetalLayer through nativeContext; GL has no equivalent and
// ignores it. Keeping the signatures identical is what lets the two swap.
//
// The create flags are the renderer's own, so they are chosen here rather than
// passed in. NVG_STENCIL_STROKES needs the stencil buffer requested in
// Window::create; without it the multi-pass glow strokes double-blend at the
// joins.

NVGcontext* canvasCreate(void* nativeContext)
{
    (void)nativeContext;
    return nvgCreateGL2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
}

void canvasDestroy(NVGcontext* ctx)
{
    if (ctx) {
        nvgDeleteGL2(ctx);
    }
}

// The only two GL calls the editor needs that NanoVG does not make for us.
// Keeping them here means no C++ translation unit has to include a GL header,
// which on Windows would mean repeating the mess above.

void canvasViewport(int w, int h) { glViewport(0, 0, w, h); }

// The context is unused here; Metal needs it, because clearing there is a
// render-pass property recorded against the context rather than a command.
void canvasClear(NVGcontext* ctx, float r, float g, float b)
{
    (void)ctx;
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

// Used only by the offline preview tool, which renders the real editor and
// writes the framebuffer out so the panel can be checked without a host.
// The size is asked of GL rather than derived from the view: whether the
// framebuffer is scaled depends on the display the window happened to open on.
void canvasViewportSize(int* w, int* h)
{
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    *w = vp[2];
    *h = vp[3];
}

void canvasReadPixels(int w, int h, unsigned char* rgb)
{
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb);
}

// A target owns its entry points and storage: simultaneous offscreen editors
// must not delete one another's renderbuffers. Windows' calling convention
// matters on 32-bit builds as well as on the 64-bit CI runners.
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#  include <dlfcn.h>
#  ifndef APIENTRY
#    define APIENTRY
#  endif
#endif

typedef void (APIENTRY *CanvasGenFn)(GLsizei, GLuint*);
typedef void (APIENTRY *CanvasDeleteFn)(GLsizei, const GLuint*);
typedef void (APIENTRY *CanvasBindFn)(GLenum, GLuint);
typedef void (APIENTRY *CanvasStorageFn)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *CanvasAttachFn)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *CanvasStatusFn)(GLenum);

struct CanvasTarget {
    CanvasGenFn genFramebuffers, genRenderbuffers;
    CanvasDeleteFn deleteFramebuffers, deleteRenderbuffers;
    CanvasBindFn bindFramebuffer, bindRenderbuffer;
    CanvasStorageFn renderbufferStorage;
    CanvasAttachFn framebufferRenderbuffer;
    CanvasStatusFn checkFramebufferStatus;
    GLuint framebuffer, buffers[2];
    NVGcontext* vg;
};

// GL is already linked; resolve the FBO functions here so the renderer seam
// does not expose a GL loader to its callers.
static void (*canvasResolve(const char* name))(void)
{
#if defined(_WIN32)
    return (void (*)(void))suppleResolveGL(name);
#else
    void* symbol = dlsym(RTLD_DEFAULT, name);
    void (*proc)(void) = NULL;
    // POSIX guarantees dlsym's conversion; memcpy also satisfies ISO C's
    // pedantic diagnostics without an object-to-function-pointer cast.
    memcpy(&proc, &symbol, sizeof(proc));
    return proc;
#endif
}

CanvasTarget* canvasTargetCreate(int w, int h, const char** why)
{
    const GLenum kFramebuffer = 0x8D40, kRenderbuffer = 0x8D41, kRgba8 = 0x8058, kDepthStencil = 0x88F0,
                 kColour0 = 0x8CE0, kDepthStencilAttachment = 0x821A, kComplete = 0x8CD5;
    CanvasTarget* target = (CanvasTarget*)calloc(1, sizeof(CanvasTarget));
    *why = "no offscreen drawing: the OpenGL context has no framebuffer objects";
    if (!target) return NULL;
    target->genFramebuffers = (CanvasGenFn)canvasResolve("glGenFramebuffers");
    target->genRenderbuffers = (CanvasGenFn)canvasResolve("glGenRenderbuffers");
    target->deleteFramebuffers = (CanvasDeleteFn)canvasResolve("glDeleteFramebuffers");
    target->deleteRenderbuffers = (CanvasDeleteFn)canvasResolve("glDeleteRenderbuffers");
    target->bindFramebuffer = (CanvasBindFn)canvasResolve("glBindFramebuffer");
    target->bindRenderbuffer = (CanvasBindFn)canvasResolve("glBindRenderbuffer");
    target->renderbufferStorage = (CanvasStorageFn)canvasResolve("glRenderbufferStorage");
    target->framebufferRenderbuffer = (CanvasAttachFn)canvasResolve("glFramebufferRenderbuffer");
    target->checkFramebufferStatus = (CanvasStatusFn)canvasResolve("glCheckFramebufferStatus");
    if (!target->genFramebuffers || !target->genRenderbuffers || !target->deleteFramebuffers ||
        !target->deleteRenderbuffers || !target->bindFramebuffer || !target->bindRenderbuffer ||
        !target->renderbufferStorage || !target->framebufferRenderbuffer || !target->checkFramebufferStatus) {
        free(target);
        return NULL;
    }
    target->genFramebuffers(1, &target->framebuffer);
    target->bindFramebuffer(kFramebuffer, target->framebuffer);
    target->genRenderbuffers(2, target->buffers);
    target->bindRenderbuffer(kRenderbuffer, target->buffers[0]);
    target->renderbufferStorage(kRenderbuffer, kRgba8, w, h);
    target->framebufferRenderbuffer(kFramebuffer, kColour0, kRenderbuffer, target->buffers[0]);
    target->bindRenderbuffer(kRenderbuffer, target->buffers[1]);
    target->renderbufferStorage(kRenderbuffer, kDepthStencil, w, h);
    target->framebufferRenderbuffer(kFramebuffer, kDepthStencilAttachment, kRenderbuffer, target->buffers[1]);
    if (target->checkFramebufferStatus(kFramebuffer) != kComplete) {
        canvasTargetDestroy(target);
        return NULL;
    }
    // Check FBO support first, so GDI Generic GL 1.1 reports the actual limit.
    if (!canvasLoad() || !(target->vg = canvasCreate(NULL))) {
        *why = "NanoVG would not start on the offscreen context";
        canvasTargetDestroy(target);
        return NULL;
    }
    return target;
}

NVGcontext* canvasTargetContext(CanvasTarget* target) { return target->vg; }

void canvasTargetDestroy(CanvasTarget* target)
{
    if (!target) return;
    canvasDestroy(target->vg);
    target->bindFramebuffer(0x8D40, 0);
    target->deleteRenderbuffers(2, target->buffers);
    target->deleteFramebuffers(1, &target->framebuffer);
    free(target);
}

int canvasReadPixelsRGBA(CanvasTarget* target, int w, int h, unsigned char* rgba)
{
    (void)target;
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    // GL reads bottom first. Swap in place, without another picture allocation.
    const size_t stride = (size_t)w * 4;
    for (int y = 0; y < h / 2; ++y) {
        unsigned char* top = rgba + (size_t)y * stride;
        unsigned char* bottom = rgba + (size_t)(h - 1 - y) * stride;
        for (size_t x = 0; x < stride; ++x) {
            const unsigned char c = top[x];
            top[x] = bottom[x];
            bottom[x] = c;
        }
    }
    return glGetError() == GL_NO_ERROR;
}
