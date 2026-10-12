/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Supple Audio */
/* dr_wav's and dr_flac's implementations, compiled once, as vendored code
   (no warnings of ours apply to them). dsp/AudioFile.cpp uses their
   declarations. Decoding only, from files: no MP3, no standard I/O
   callbacks of our own. */
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
