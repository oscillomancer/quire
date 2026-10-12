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

#ifdef __cplusplus
}
#endif
