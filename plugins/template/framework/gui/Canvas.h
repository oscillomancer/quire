/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Supple Audio
 *
 * The renderer seam. Exactly one of CanvasGL.c and CanvasMetal.m is linked
 * into a binary and implements these, which is what lets Window.cpp name no
 * renderer and include no GL or Metal header.
 */
#pragma once

#include <nanovg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* nativeContext is whatever puglGetContext() returns: ignored under GL, the
 * CAMetalLayer under Metal. Call with the view's context current. */
NVGcontext* canvasCreate(void* nativeContext);
void canvasDestroy(NVGcontext* ctx);
/* Resolves GL entry points where the platform requires it (Windows). */
int canvasLoad(void);
void canvasViewport(int width, int height);
void canvasClear(NVGcontext* ctx, float r, float g, float b);

/* Offline preview only: the framebuffer size and contents. GL only. */
void canvasViewportSize(int* width, int* height);
void canvasReadPixels(int width, int height, unsigned char* rgb);

/* Drawing without a window (gui/Offscreen.cpp), GL only: a framebuffer
 * object width x height, colour and stencil, bound for drawing; its entry
 * points come from `resolve` (eglGetProcAddress). 0 when the context has no
 * framebuffer objects. canvasReadPixelsRGBA reads what is bound, bottom row
 * first, as GL does. */
typedef void (*CanvasProc)(void);
unsigned canvasTargetCreate(int width, int height, CanvasProc (*resolve)(const char* name));
void canvasTargetDestroy(unsigned target);
void canvasReadPixelsRGBA(int width, int height, unsigned char* rgba);

#ifdef __cplusplus
}
#endif
