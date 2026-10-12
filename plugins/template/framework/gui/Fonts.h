// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The embedded faces (see font:: in Theme.h). A plugin has nowhere reliable
// to load a font file from -- bundle layouts differ per format and host -- so
// they are compiled in by cmake/Bin2c.cmake.
#pragma once

#include <cstddef>

#include <nanovg.h>

namespace supple::gui {

// Registers every face with this context: the framework's (see font:: in
// Theme.h), and any .ttf or .otf among the product's assets under its file
// stem. Plex Mono, the display face and the product's fall back to Inter
// for any glyph they lack.
bool loadFonts(NVGcontext* vg);

// One face from TTF or OTF bytes that outlive the context, after
// loadFonts(): falling back to Inter for glyphs it lacks, and registered as
// Inter itself when the bytes will not load, so text set in it still draws.
bool loadFace(NVGcontext* vg, const char* name, const unsigned char* data, size_t size);

}  // namespace supple::gui
