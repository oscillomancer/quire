// SPDX-License-Identifier: MIT AND ISC
// Copyright (c) 2026 Supple Audio
// Derived in part from pugl's src/mac_vulkan.m,
// Copyright 2012-2022 David Robillard <d@drobilla.net>, ISC license
// (full text in NOTICES.txt).
//
//
// A Metal backend for pugl.
//
// pugl ships backends for GL, Cairo and Vulkan but not Metal, and upstream has
// no open work towards one. This supplies it, modelled directly on pugl's own
// src/mac_vulkan.m -- which already builds a CAMetalLayer-backed view for
// MoltenVK, and is therefore most of a Metal backend with a Vulkan surface
// bolted on top. The layer is all NanoVG's Metal renderer needs.
//
// puglGetContext() returns the CAMetalLayer, mirroring how pugl's Cairo
// backend returns the cairo_t: the backend hands back whatever object the
// drawing library expects.
#pragma once

#include <pugl/pugl.h>

PUGL_BEGIN_DECLS

PUGL_API const PuglBackend* puglMetalBackend(void);

PUGL_END_DECLS
