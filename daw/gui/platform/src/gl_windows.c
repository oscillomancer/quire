// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <SDL3/SDL.h>
#include "supple/gui/gl_api.h"

PFNGLACTIVETEXTUREPROC supple_glActiveTexture;
PFNGLATTACHSHADERPROC supple_glAttachShader;
PFNGLBINDATTRIBLOCATIONPROC supple_glBindAttribLocation;
PFNGLBINDBUFFERPROC supple_glBindBuffer;
PFNGLBINDBUFFERRANGEPROC supple_glBindBufferRange;
PFNGLBINDFRAMEBUFFERPROC supple_glBindFramebuffer;
PFNGLBINDRENDERBUFFERPROC supple_glBindRenderbuffer;
PFNGLBINDVERTEXARRAYPROC supple_glBindVertexArray;
PFNGLBLENDFUNCSEPARATEPROC supple_glBlendFuncSeparate;
PFNGLBLITFRAMEBUFFERPROC supple_glBlitFramebuffer;
PFNGLBUFFERDATAPROC supple_glBufferData;
PFNGLBUFFERSUBDATAPROC supple_glBufferSubData;
PFNGLCHECKFRAMEBUFFERSTATUSPROC supple_glCheckFramebufferStatus;
PFNGLCOMPILESHADERPROC supple_glCompileShader;
PFNGLCREATEPROGRAMPROC supple_glCreateProgram;
PFNGLCREATESHADERPROC supple_glCreateShader;
PFNGLDELETEBUFFERSPROC supple_glDeleteBuffers;
PFNGLDELETEFRAMEBUFFERSPROC supple_glDeleteFramebuffers;
PFNGLDELETEPROGRAMPROC supple_glDeleteProgram;
PFNGLDELETERENDERBUFFERSPROC supple_glDeleteRenderbuffers;
PFNGLDELETESHADERPROC supple_glDeleteShader;
PFNGLDELETEVERTEXARRAYSPROC supple_glDeleteVertexArrays;
PFNGLDISABLEVERTEXATTRIBARRAYPROC supple_glDisableVertexAttribArray;
PFNGLDRAWARRAYSINSTANCEDPROC supple_glDrawArraysInstanced;
PFNGLENABLEVERTEXATTRIBARRAYPROC supple_glEnableVertexAttribArray;
PFNGLFRAMEBUFFERRENDERBUFFERPROC supple_glFramebufferRenderbuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC supple_glFramebufferTexture2D;
PFNGLGENBUFFERSPROC supple_glGenBuffers;
PFNGLGENFRAMEBUFFERSPROC supple_glGenFramebuffers;
PFNGLGENRENDERBUFFERSPROC supple_glGenRenderbuffers;
PFNGLGENVERTEXARRAYSPROC supple_glGenVertexArrays;
PFNGLGENERATEMIPMAPPROC supple_glGenerateMipmap;
PFNGLGETPROGRAMINFOLOGPROC supple_glGetProgramInfoLog;
PFNGLGETPROGRAMIVPROC supple_glGetProgramiv;
PFNGLGETSHADERINFOLOGPROC supple_glGetShaderInfoLog;
PFNGLGETSHADERIVPROC supple_glGetShaderiv;
PFNGLGETUNIFORMBLOCKINDEXPROC supple_glGetUniformBlockIndex;
PFNGLGETUNIFORMLOCATIONPROC supple_glGetUniformLocation;
PFNGLLINKPROGRAMPROC supple_glLinkProgram;
PFNGLMULTIDRAWARRAYSPROC supple_glMultiDrawArrays;
PFNGLRENDERBUFFERSTORAGEPROC supple_glRenderbufferStorage;
PFNGLSHADERSOURCEPROC supple_glShaderSource;
PFNGLSTENCILOPSEPARATEPROC supple_glStencilOpSeparate;
PFNGLUNIFORM1FPROC supple_glUniform1f;
PFNGLUNIFORM1IPROC supple_glUniform1i;
PFNGLUNIFORM2FVPROC supple_glUniform2fv;
PFNGLUNIFORM4FPROC supple_glUniform4f;
PFNGLUNIFORM4FVPROC supple_glUniform4fv;
PFNGLUNIFORMBLOCKBINDINGPROC supple_glUniformBlockBinding;
PFNGLUSEPROGRAMPROC supple_glUseProgram;
PFNGLVERTEXATTRIBDIVISORPROC supple_glVertexAttribDivisor;
PFNGLVERTEXATTRIBPOINTERPROC supple_glVertexAttribPointer;

int suppleLoadGl(void) {
    supple_glActiveTexture = (PFNGLACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glActiveTexture");
    if (!supple_glActiveTexture) return 0;
    supple_glAttachShader = (PFNGLATTACHSHADERPROC)SDL_GL_GetProcAddress("glAttachShader");
    if (!supple_glAttachShader) return 0;
    supple_glBindAttribLocation =
        (PFNGLBINDATTRIBLOCATIONPROC)SDL_GL_GetProcAddress("glBindAttribLocation");
    if (!supple_glBindAttribLocation) return 0;
    supple_glBindBuffer = (PFNGLBINDBUFFERPROC)SDL_GL_GetProcAddress("glBindBuffer");
    if (!supple_glBindBuffer) return 0;
    supple_glBindBufferRange = (PFNGLBINDBUFFERRANGEPROC)SDL_GL_GetProcAddress("glBindBufferRange");
    if (!supple_glBindBufferRange) return 0;
    supple_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBindFramebuffer");
    if (!supple_glBindFramebuffer) return 0;
    supple_glBindRenderbuffer =
        (PFNGLBINDRENDERBUFFERPROC)SDL_GL_GetProcAddress("glBindRenderbuffer");
    if (!supple_glBindRenderbuffer) return 0;
    supple_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)SDL_GL_GetProcAddress("glBindVertexArray");
    if (!supple_glBindVertexArray) return 0;
    supple_glBlendFuncSeparate =
        (PFNGLBLENDFUNCSEPARATEPROC)SDL_GL_GetProcAddress("glBlendFuncSeparate");
    if (!supple_glBlendFuncSeparate) return 0;
    supple_glBlitFramebuffer = (PFNGLBLITFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBlitFramebuffer");
    if (!supple_glBlitFramebuffer) return 0;
    supple_glBufferData = (PFNGLBUFFERDATAPROC)SDL_GL_GetProcAddress("glBufferData");
    if (!supple_glBufferData) return 0;
    supple_glBufferSubData = (PFNGLBUFFERSUBDATAPROC)SDL_GL_GetProcAddress("glBufferSubData");
    if (!supple_glBufferSubData) return 0;
    supple_glCheckFramebufferStatus =
        (PFNGLCHECKFRAMEBUFFERSTATUSPROC)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    if (!supple_glCheckFramebufferStatus) return 0;
    supple_glCompileShader = (PFNGLCOMPILESHADERPROC)SDL_GL_GetProcAddress("glCompileShader");
    if (!supple_glCompileShader) return 0;
    supple_glCreateProgram = (PFNGLCREATEPROGRAMPROC)SDL_GL_GetProcAddress("glCreateProgram");
    if (!supple_glCreateProgram) return 0;
    supple_glCreateShader = (PFNGLCREATESHADERPROC)SDL_GL_GetProcAddress("glCreateShader");
    if (!supple_glCreateShader) return 0;
    supple_glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteBuffers");
    if (!supple_glDeleteBuffers) return 0;
    supple_glDeleteFramebuffers =
        (PFNGLDELETEFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteFramebuffers");
    if (!supple_glDeleteFramebuffers) return 0;
    supple_glDeleteProgram = (PFNGLDELETEPROGRAMPROC)SDL_GL_GetProcAddress("glDeleteProgram");
    if (!supple_glDeleteProgram) return 0;
    supple_glDeleteRenderbuffers =
        (PFNGLDELETERENDERBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteRenderbuffers");
    if (!supple_glDeleteRenderbuffers) return 0;
    supple_glDeleteShader = (PFNGLDELETESHADERPROC)SDL_GL_GetProcAddress("glDeleteShader");
    if (!supple_glDeleteShader) return 0;
    supple_glDeleteVertexArrays =
        (PFNGLDELETEVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glDeleteVertexArrays");
    if (!supple_glDeleteVertexArrays) return 0;
    supple_glDisableVertexAttribArray =
        (PFNGLDISABLEVERTEXATTRIBARRAYPROC)SDL_GL_GetProcAddress("glDisableVertexAttribArray");
    if (!supple_glDisableVertexAttribArray) return 0;
    supple_glDrawArraysInstanced =
        (PFNGLDRAWARRAYSINSTANCEDPROC)SDL_GL_GetProcAddress("glDrawArraysInstanced");
    if (!supple_glDrawArraysInstanced) return 0;
    supple_glEnableVertexAttribArray =
        (PFNGLENABLEVERTEXATTRIBARRAYPROC)SDL_GL_GetProcAddress("glEnableVertexAttribArray");
    if (!supple_glEnableVertexAttribArray) return 0;
    supple_glFramebufferRenderbuffer =
        (PFNGLFRAMEBUFFERRENDERBUFFERPROC)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
    if (!supple_glFramebufferRenderbuffer) return 0;
    supple_glFramebufferTexture2D =
        (PFNGLFRAMEBUFFERTEXTURE2DPROC)SDL_GL_GetProcAddress("glFramebufferTexture2D");
    if (!supple_glFramebufferTexture2D) return 0;
    supple_glGenBuffers = (PFNGLGENBUFFERSPROC)SDL_GL_GetProcAddress("glGenBuffers");
    if (!supple_glGenBuffers) return 0;
    supple_glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glGenFramebuffers");
    if (!supple_glGenFramebuffers) return 0;
    supple_glGenRenderbuffers =
        (PFNGLGENRENDERBUFFERSPROC)SDL_GL_GetProcAddress("glGenRenderbuffers");
    if (!supple_glGenRenderbuffers) return 0;
    supple_glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glGenVertexArrays");
    if (!supple_glGenVertexArrays) return 0;
    supple_glGenerateMipmap = (PFNGLGENERATEMIPMAPPROC)SDL_GL_GetProcAddress("glGenerateMipmap");
    if (!supple_glGenerateMipmap) return 0;
    supple_glGetProgramInfoLog =
        (PFNGLGETPROGRAMINFOLOGPROC)SDL_GL_GetProcAddress("glGetProgramInfoLog");
    if (!supple_glGetProgramInfoLog) return 0;
    supple_glGetProgramiv = (PFNGLGETPROGRAMIVPROC)SDL_GL_GetProcAddress("glGetProgramiv");
    if (!supple_glGetProgramiv) return 0;
    supple_glGetShaderInfoLog =
        (PFNGLGETSHADERINFOLOGPROC)SDL_GL_GetProcAddress("glGetShaderInfoLog");
    if (!supple_glGetShaderInfoLog) return 0;
    supple_glGetShaderiv = (PFNGLGETSHADERIVPROC)SDL_GL_GetProcAddress("glGetShaderiv");
    if (!supple_glGetShaderiv) return 0;
    supple_glGetUniformBlockIndex =
        (PFNGLGETUNIFORMBLOCKINDEXPROC)SDL_GL_GetProcAddress("glGetUniformBlockIndex");
    if (!supple_glGetUniformBlockIndex) return 0;
    supple_glGetUniformLocation =
        (PFNGLGETUNIFORMLOCATIONPROC)SDL_GL_GetProcAddress("glGetUniformLocation");
    if (!supple_glGetUniformLocation) return 0;
    supple_glLinkProgram = (PFNGLLINKPROGRAMPROC)SDL_GL_GetProcAddress("glLinkProgram");
    if (!supple_glLinkProgram) return 0;
    supple_glMultiDrawArrays = (PFNGLMULTIDRAWARRAYSPROC)SDL_GL_GetProcAddress("glMultiDrawArrays");
    if (!supple_glMultiDrawArrays) return 0;
    supple_glRenderbufferStorage =
        (PFNGLRENDERBUFFERSTORAGEPROC)SDL_GL_GetProcAddress("glRenderbufferStorage");
    if (!supple_glRenderbufferStorage) return 0;
    supple_glShaderSource = (PFNGLSHADERSOURCEPROC)SDL_GL_GetProcAddress("glShaderSource");
    if (!supple_glShaderSource) return 0;
    supple_glStencilOpSeparate =
        (PFNGLSTENCILOPSEPARATEPROC)SDL_GL_GetProcAddress("glStencilOpSeparate");
    if (!supple_glStencilOpSeparate) return 0;
    supple_glUniform1f = (PFNGLUNIFORM1FPROC)SDL_GL_GetProcAddress("glUniform1f");
    if (!supple_glUniform1f) return 0;
    supple_glUniform1i = (PFNGLUNIFORM1IPROC)SDL_GL_GetProcAddress("glUniform1i");
    if (!supple_glUniform1i) return 0;
    supple_glUniform2fv = (PFNGLUNIFORM2FVPROC)SDL_GL_GetProcAddress("glUniform2fv");
    if (!supple_glUniform2fv) return 0;
    supple_glUniform4f = (PFNGLUNIFORM4FPROC)SDL_GL_GetProcAddress("glUniform4f");
    if (!supple_glUniform4f) return 0;
    supple_glUniform4fv = (PFNGLUNIFORM4FVPROC)SDL_GL_GetProcAddress("glUniform4fv");
    if (!supple_glUniform4fv) return 0;
    supple_glUniformBlockBinding =
        (PFNGLUNIFORMBLOCKBINDINGPROC)SDL_GL_GetProcAddress("glUniformBlockBinding");
    if (!supple_glUniformBlockBinding) return 0;
    supple_glUseProgram = (PFNGLUSEPROGRAMPROC)SDL_GL_GetProcAddress("glUseProgram");
    if (!supple_glUseProgram) return 0;
    supple_glVertexAttribDivisor =
        (PFNGLVERTEXATTRIBDIVISORPROC)SDL_GL_GetProcAddress("glVertexAttribDivisor");
    if (!supple_glVertexAttribDivisor) return 0;
    supple_glVertexAttribPointer =
        (PFNGLVERTEXATTRIBPOINTERPROC)SDL_GL_GetProcAddress("glVertexAttribPointer");
    if (!supple_glVertexAttribPointer) return 0;
    return 1;
}
