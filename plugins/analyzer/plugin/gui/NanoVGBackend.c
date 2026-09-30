// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The single translation unit that instantiates NanoVG's OpenGL backend.
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

#  define WIN32_LEAN_AND_MEAN
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

// name, return type, parameter list -- one row per entry point NanoVG's GL2
// backend calls above GL 1.1.
#  define ANALYZER_GL_PROCS                                                     \
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

#  define X(name, ret, params) typedef ret(APIENTRY *analyzer_pfn_##name) params;
ANALYZER_GL_PROCS
#  undef X

#  define X(name, ret, params) static analyzer_pfn_##name analyzer_##name;
ANALYZER_GL_PROCS
#  undef X

// wglGetProcAddress does not signal failure with NULL alone: the documented
// failure values are 0, 1, 2, 3 and -1, and drivers have historically returned
// the small integers. Treating one of those as a function pointer crashes on
// the first call rather than failing here, where the caller already has a
// clean path for a context it cannot use.
static PROC analyzerResolveGL(const char* name)
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

int analyzerLoadGL(void)
{
#  define X(name, ret, params)                                                 \
    analyzer_##name = (analyzer_pfn_##name)analyzerResolveGL(#name);                \
    if (!analyzer_##name) return 0;
    ANALYZER_GL_PROCS
#  undef X
    return 1;
}

// Redirect the names onto the resolved pointers for everything below.
#  define glActiveTexture            analyzer_glActiveTexture
#  define glAttachShader             analyzer_glAttachShader
#  define glBindAttribLocation       analyzer_glBindAttribLocation
#  define glBindBuffer               analyzer_glBindBuffer
#  define glBlendFuncSeparate        analyzer_glBlendFuncSeparate
#  define glBufferData               analyzer_glBufferData
#  define glCompileShader            analyzer_glCompileShader
#  define glCreateProgram            analyzer_glCreateProgram
#  define glCreateShader             analyzer_glCreateShader
#  define glDeleteBuffers            analyzer_glDeleteBuffers
#  define glDeleteProgram            analyzer_glDeleteProgram
#  define glDeleteShader             analyzer_glDeleteShader
#  define glDisableVertexAttribArray analyzer_glDisableVertexAttribArray
#  define glEnableVertexAttribArray  analyzer_glEnableVertexAttribArray
#  define glGenBuffers               analyzer_glGenBuffers
#  define glGenerateMipmap           analyzer_glGenerateMipmap
#  define glGetProgramInfoLog        analyzer_glGetProgramInfoLog
#  define glGetProgramiv             analyzer_glGetProgramiv
#  define glGetShaderInfoLog         analyzer_glGetShaderInfoLog
#  define glGetShaderiv              analyzer_glGetShaderiv
#  define glGetUniformLocation       analyzer_glGetUniformLocation
#  define glLinkProgram              analyzer_glLinkProgram
#  define glShaderSource             analyzer_glShaderSource
#  define glStencilOpSeparate        analyzer_glStencilOpSeparate
#  define glUniform1i                analyzer_glUniform1i
#  define glUniform2fv               analyzer_glUniform2fv
#  define glUniform4fv               analyzer_glUniform4fv
#  define glUseProgram               analyzer_glUseProgram
#  define glVertexAttribPointer      analyzer_glVertexAttribPointer

#elif defined(__APPLE__)

#  ifndef GL_SILENCE_DEPRECATION  // also set on the pugl target
#    define GL_SILENCE_DEPRECATION
#  endif
#  include <OpenGL/gl.h>

int analyzerLoadGL(void) { return 1; }

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

int analyzerLoadGL(void) { return 1; }

#endif

#define NANOVG_GL2_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_gl.h"

// Context creation lives behind the same seam as everything else here, so that
// Window.cpp names no renderer at all. The Metal backend's version of this
// takes the CAMetalLayer through nativeContext; GL has no equivalent and
// ignores it. Keeping the signatures identical is what lets the two swap.
//
// The create flags are the renderer's own, so they are chosen here rather than
// passed in. NVG_STENCIL_STROKES needs the stencil buffer requested in
// Window::create; without it the multi-pass glow strokes double-blend at the
// joins.

NVGcontext* analyzerNvgCreate(void* nativeContext)
{
    (void)nativeContext;
    return nvgCreateGL2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
}

void analyzerNvgDelete(NVGcontext* ctx)
{
    if (ctx) {
        nvgDeleteGL2(ctx);
    }
}

// The only two GL calls the editor needs that NanoVG does not make for us.
// Keeping them here means no C++ translation unit has to include a GL header,
// which on Windows would mean repeating the mess above.

void analyzerViewport(int w, int h) { glViewport(0, 0, w, h); }

// The context is unused here; Metal needs it, because clearing there is a
// render-pass property recorded against the context rather than a command.
void analyzerClear(NVGcontext* ctx, float r, float g, float b)
{
    (void)ctx;
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

// Used only by the offline preview tool, which renders the real editor and
// writes the framebuffer out so the panel can be checked without a host.
// The size is asked of GL rather than derived from the view: whether the
// framebuffer is scaled depends on the display the window happened to open on.
void analyzerViewportSize(int* w, int* h)
{
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    *w = vp[2];
    *h = vp[3];
}

void analyzerReadPixels(int w, int h, unsigned char* rgb)
{
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb);
}
