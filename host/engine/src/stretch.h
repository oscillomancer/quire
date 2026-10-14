// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Internal: pitch-preserving time-stretch of a whole media file (main
// thread). Used by the media cache for takes with `preserve_pitch` and the
// stretch pitch mode (engine.md §4); not a public interface.
#pragma once

#include "supple/audio/audio_file.h"
#include "supple/base/result.h"

namespace supple::engine {

// `in` stretched to `in.frames() / playrate` frames at its own rate, with the
// pitch preserved and transposed by `pitchSemitones`. Deterministic: the same
// input and parameters always give the same samples (the stretch's phase
// randomization is seeded), so a media variant is identical in every cache.
Result<audio::AudioData> stretchAudio(const audio::AudioData& in, double playrate,
                                      double pitchSemitones);

}  // namespace supple::engine
