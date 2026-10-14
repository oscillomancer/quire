// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
/* The single translation unit that compiles the vendored dr_libs (public
 * domain / MIT-0). Warnings are disabled for this file only. */
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_MP3_NO_STDIO
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
