// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The Metal half of the renderer seam (gui/Canvas.h).
//
// The sibling of CanvasGL.c, implementing the same canvas* seam so that
// Window.cpp never learns which renderer it is driving. Exactly one of the two
// is compiled into any given binary; GL remains the only option on Linux and
// Windows, and macOS chooses at configure time.
//
// The renderer is wjakob/nanovg_metal rather than ollix/MetalNanoVG: the
// original has not been touched since 2022, while the fork carries the three
// changes this integration actually depends on -- an explicit command queue,
// an explicit target texture, and a stencil buffer sized from the render
// target (which its fills need).

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "nanovg.h"
#include "nanovg_mtl.h"

#include "gui/Canvas.h"

// Nothing in here is file-static on purpose. A DAW routinely runs several
// instances of a plugin at once, so anything kept process-wide is shared
// between editors that know nothing about each other -- and tearing one down
// would then pull the queue, or the clear target, out from under the others.
// Everything below is either per-context or passed in.

// Signature matches the GL backend's. nativeContext is the CAMetalLayer that
// puglMetalBackend's getContext hands back. The create flags are chosen here
// rather than passed in: their values and their meanings are the renderer's,
// and the caller has no business knowing either.
//
// No NVG_STENCIL_STROKES, unlike the GL backend, because nanovg_metal's
// version of it is broken. Stencil strokes draw each stroke twice: a first
// pass meant to touch only fully covered pixels, marking them in the
// stencil, then the antialiased pass everywhere the stencil is still clear.
// The first pass relies on the fragment shader *discarding* partially
// covered pixels; GL's shader does (`discard`), but nanovg_mtl_shaders.metal
// returns transparent black instead, which still increments the stencil. So
// every partially covered pixel is marked by an invisible draw and then
// skipped by the visible one: every stroke loses its antialiased edge, and a
// one-unit line that lands between pixels -- most of them, at any
// non-integer editor scale -- vanishes entirely. Seen in CI captures at
// 1.25x and 1.56x, where GL draws the same grid lines. The shaders ship
// precompiled, so fixing them means owning a Metal compile step; dropping
// the flag costs only the double-blended joins of translucent multi-segment
// strokes, including translucent plots and waveform traces.
//
// NVG_TRIPLE_BUFFER matters. Without a buffering flag nanovg_mtl allocates one
// buffer set and a semaphore of one, so renderViewport blocks until the GPU has
// finished the previous frame -- on the main thread, which in a plugin is the
// host's UI thread. Three lets the CPU run ahead instead.
static const int kCanvasFlags = NVG_ANTIALIAS | NVG_TRIPLE_BUFFER;

NVGcontext* canvasCreate(void* nativeContext)
{
    CAMetalLayer* layer = (__bridge CAMetalLayer*)nativeContext;
    if (!layer) {
        return NULL;
    }

    // The layer has no device until someone assigns one; pugl deliberately
    // does not choose a GPU on our behalf. This file is not ARC, and
    // MTLCreateSystemDefaultDevice follows the Create rule, so the reference it
    // returns is ours to drop once the layer has retained it.
    if (!layer.device) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        layer.device = device;
        [device release];
    }
    if (!layer.device) {
        return NULL;
    }

    // nanovg_mtl holds the queue in a strong property, so hand it over and drop
    // our own reference: the context owns it from here, and nvgDeleteMTL is
    // what finally releases it.
    id<MTLCommandQueue> queue = [layer.device newCommandQueue];
    if (!queue) {
        return NULL;
    }

    NVGcontext* ctx = nvgCreateMTL((__bridge void*)layer, (__bridge void*)queue, kCanvasFlags);
    [queue release];
    return ctx;
}

void canvasDestroy(NVGcontext* ctx)
{
    if (ctx) {
        nvgDeleteMTL(ctx);
    }
}

// GL needs its entry points resolved before first use; Metal does not. The
// seam keeps the call so Window.cpp stays identical across backends.
int canvasLoad(void) { return 1; }

// Under GL this is glViewport. Metal derives the viewport from the drawable,
// which pugl_metal.m keeps sized to the view, so there is nothing to set --
// and setting it here would fight that.
void canvasViewport(int w, int h)
{
    (void)w;
    (void)h;
}

// Clearing under Metal is a property of the render pass rather than a command,
// so this records the colour against the context and nanovg_mtl applies it when
// the frame is flushed. It must be called before nvgBeginFrame, which is where
// the GL backend wants it too. The context is passed rather than remembered,
// so two editors cannot clear each other's frames.
void canvasClear(NVGcontext* ctx, float r, float g, float b)
{
    if (ctx) {
        mnvgClearWithColor(ctx, nvgRGBAf(r, g, b, 1.0f));
    }
}

// Both of these exist only for the offline preview tool, which always builds
// against the GL backend (see the gui-preview target in framework/cmake):
// it reads the framebuffer back, and doing that under Metal means rendering
// into an MNVGframebuffer rather than the layer's drawable -- a different draw
// path for a tool whose whole job is to exercise the normal one. Left
// deliberately unimplemented rather than half-implemented, and rather than
// kept alive by a file-static layer pointer that would outlive its editor.
// Nothing in the plugin calls either.
void canvasViewportSize(int* w, int* h)
{
    *w = 0;
    *h = 0;
}

void canvasReadPixels(int w, int h, unsigned char* rgb)
{
    (void)w;
    (void)h;
    (void)rgb;
}

// Headless drawing uses the same renderer and flags as an on-screen editor.
// Each target keeps an autorelease pool for the entire worker-thread lifetime.
#include <stdlib.h>
#include <string.h>

struct CanvasTarget {
    NSAutoreleasePool* pool;
    NVGcontext* vg;
    MNVGframebuffer* framebuffer;
    id<MTLCommandQueue> queue;
};

CanvasTarget* canvasTargetCreate(int w, int h, const char** why)
{
    CanvasTarget* target = (CanvasTarget*)calloc(1, sizeof(CanvasTarget));
    *why = "no offscreen drawing: Metal could not allocate a render target";
    if (!target) return NULL;
    target->pool = [[NSAutoreleasePool alloc] init];
    CAMetalLayer* layer = [[CAMetalLayer alloc] init];
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    layer.device = device;
    [device release];
    if (!layer.device) {
        *why = "no offscreen drawing: Metal found no device";
        [layer release];
        canvasTargetDestroy(target);
        return NULL;
    }
    target->queue = [layer.device newCommandQueue];
    if (target->queue)
        target->vg = nvgCreateMTL((__bridge void*)layer, (__bridge void*)target->queue,
                                  kCanvasFlags);
    [layer release];  // retained by NanoVG
    if (!target->vg) {
        *why = "NanoVG would not start on the offscreen context";
        canvasTargetDestroy(target);
        return NULL;
    }
    target->framebuffer = mnvgCreateFramebuffer(target->vg, w, h, 0);
    if (!target->framebuffer || target->framebuffer->image <= 0) {
        canvasTargetDestroy(target);
        return NULL;
    }
    // Avoid mnvgBindFramebuffer: the dependency keeps its binding in a global,
    // which races visible editors and other previews. This fork also offers a
    // per-context texture target. Readback below handles synchronization itself.
    mnvgSetColorTexture(target->vg, mnvgImageHandle(target->vg, target->framebuffer->image));
    return target;
}

NVGcontext* canvasTargetContext(CanvasTarget* target) { return target->vg; }

void canvasTargetDestroy(CanvasTarget* target)
{
    if (!target) return;
    mnvgDeleteFramebuffer(target->framebuffer);
    canvasDestroy(target->vg);
    [target->queue release];
    [target->pool drain];
    free(target);
}

int canvasReadPixelsRGBA(CanvasTarget* target, int w, int h, unsigned char* rgba)
{
    // Framebuffer textures are RGBA8Unorm, unlike BGRA layer drawables. Copy
    // on the rendering queue, then wait: shared staging storage works on both
    // unified-memory and discrete GPUs, without managed-texture assumptions.
    id<MTLTexture> texture = (__bridge id<MTLTexture>)mnvgImageHandle(target->vg, target->framebuffer->image);
    const NSUInteger stride = ((NSUInteger)w * 4 + 255) & ~(NSUInteger)255;
    id<MTLBuffer> buffer = [texture.device newBufferWithLength:stride * (NSUInteger)h
                                                     options:MTLResourceStorageModeShared];
    if (!buffer) return 0;
    id<MTLCommandBuffer> command = [target->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                 toBuffer:buffer destinationOffset:0 destinationBytesPerRow:stride
  destinationBytesPerImage:stride * (NSUInteger)h];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (buffer && command.status == MTLCommandBufferStatusCompleted) {
        for (int y = 0; y < h; ++y)
            memcpy(rgba + (size_t)y * (size_t)w * 4,
                   (const unsigned char*)buffer.contents + (NSUInteger)y * stride, (size_t)w * 4);
    }
    const int ok = command.status == MTLCommandBufferStatusCompleted;
    [buffer release];
    return ok;
}
