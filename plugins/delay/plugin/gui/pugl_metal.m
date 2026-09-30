// SPDX-License-Identifier: MIT AND ISC
// Copyright (c) 2026 Supple Audio
// Derived in part from pugl's src/mac_vulkan.m,
// Copyright 2012-2022 David Robillard <d@drobilla.net>, ISC license
// (full text in NOTICES.txt).
//
//
// A Metal backend for pugl. See pugl_metal.h for why this exists.
//
// This is pugl's src/mac_vulkan.m with the Vulkan surface removed and the
// drawable sizing made explicit. Structure, naming and retain/release
// conventions follow that file deliberately: it is the closest thing upstream
// has to a Metal backend, and staying close to it keeps this cheap to re-sync
// if pugl ever ships its own.

#include "internal.h"
#include "mac.h"
#include "stub.h"
#include "types.h"

#include <pugl/pugl.h>
#include <pugl/stub.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include "pugl_metal.h"

@interface PuglMetalView : NSView<CALayerDelegate>

@end

@implementation PuglMetalView {
@public
  PuglView* puglview;
}

- (id)initWithFrame:(NSRect)frame
{
  self = [super initWithFrame:frame];

  if (self) {
    self.wantsLayer                = YES;
    self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
  }

  return self;
}

- (CALayer*)makeBackingLayer
{
  CAMetalLayer* layer = [CAMetalLayer layer];
  [layer setDelegate:self];
  return layer;
}

// A CAMetalLayer does not size its drawable from its bounds, so both the
// content scale and the drawable have to be set by hand. Getting this wrong is
// the Metal equivalent of a bad glViewport: the panel renders into a corner.
- (void)updateDrawableSize
{
  CAMetalLayer* layer = (CAMetalLayer*)self.layer;
  if (![layer isKindOfClass:[CAMetalLayer class]]) {
    return;
  }

  const CGFloat scale =
    self.window ? [self.window backingScaleFactor] : layer.contentsScale;
  const CGSize size = self.bounds.size;

  layer.contentsScale = scale;
  layer.frame         = self.bounds;
  layer.drawableSize =
    CGSizeMake(size.width * scale, size.height * scale);
}

- (void)setFrameSize:(NSSize)newSize
{
  PuglWrapperView* wrapper = (PuglWrapperView*)[self superview];

  [super setFrameSize:newSize];
  [wrapper setReshaped];

  [self updateDrawableSize];
}

// When a view is embedded in a host (view->parent set, which is every plugin
// case) pugl adds the wrapper to the parent and never sets this view's frame:
// it passes lastConfigure to the backend's create, which is still zero that
// early, and then relies on the autoresizing mask to track later changes that
// never come. An NSOpenGLView survives that because its drawable follows the
// view; a CAMetalLayer does not, because its drawable size is explicit -- it
// stays 0x0, nextDrawable returns nil, and every frame is silently dropped.
// Taking the wrapper's bounds on adoption is what closes that gap.
- (void)viewDidMoveToSuperview
{
  [super viewDidMoveToSuperview];

  if (self.superview && (self.bounds.size.width <= 0.0 ||
                         self.bounds.size.height <= 0.0)) {
    [self setFrame:self.superview.bounds];
  }

  [self updateDrawableSize];
}

// pugl sets the frame outright when it owns the window, rather than going
// through setFrameSize:.
- (void)setFrame:(NSRect)frame
{
  [super setFrame:frame];
  [self updateDrawableSize];
}

// Dragging the window between displays of different densities changes the
// backing scale without changing the frame, so the drawable must follow.
- (void)viewDidChangeBackingProperties
{
  [super viewDidChangeBackingProperties];
  [self updateDrawableSize];
}

- (void)displayLayer:(CALayer*)layer
{
  (void)layer;
  PuglWrapperView* wrapper = (PuglWrapperView*)[self superview];
  [wrapper dispatchExpose:[self bounds]];
}

// A CALayer holds its delegate unretained, and CoreAnimation can outlive this
// view: the layer is retained by the render tree, and a display may already be
// queued when we go away. Anything less than clearing the delegate on the way
// out leaves CA free to send displayLayer: to freed memory, which lands as
// dispatchExpose: on a stale pugl view and takes the whole editor with it.
// Done here as well as in the backend's destroy so that it holds whichever
// way the teardown runs -- pugl freeing the view, or AppKit releasing the
// hierarchy out from under it.
- (void)dealloc
{
  [self.layer setDelegate:nil];
  [super dealloc];
}

@end

static PuglStatus
puglMacMetalCreate(PuglView* view)
{
  PuglInternals* impl     = view->impl;
  PuglMetalView* drawView = [PuglMetalView alloc];
  const NSRect   rect =
    NSMakeRect(0, 0, view->lastConfigure.width, view->lastConfigure.height);

  drawView->puglview = view;
  [drawView initWithFrame:rect];
  if (view->hints[PUGL_RESIZABLE]) {
    [drawView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  } else {
    [drawView setAutoresizingMask:NSViewNotSizable];
  }

  [drawView updateDrawableSize];

  impl->drawView = drawView;
  return PUGL_SUCCESS;
}

static void
puglMacMetalDestroy(PuglView* view)
{
  PuglMetalView* const drawView = (PuglMetalView*)view->impl->drawView;

  // Before anything else: stop CoreAnimation being able to call back into a
  // view that is about to go away. See -dealloc above.
  [drawView.layer setDelegate:nil];
  [drawView removeFromSuperview];
  [drawView release];

  view->impl->drawView = nil;
}

// Hand back the CAMetalLayer, which is what nvgCreateMTL wants. pugl's Cairo
// backend returns the cairo_t from here for the same reason.
static void*
puglMacMetalGetContext(PuglView* view)
{
  PuglMetalView* const drawView = (PuglMetalView*)view->impl->drawView;

  return drawView ? (void*)drawView.layer : NULL;
}

const PuglBackend*
puglMetalBackend(void)
{
  static const PuglBackend backend = {puglStubConfigure,
                                      puglMacMetalCreate,
                                      puglMacMetalDestroy,
                                      puglStubEnter,
                                      puglStubLeave,
                                      puglMacMetalGetContext};

  return &backend;
}
