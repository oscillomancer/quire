// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <functional>
#include "supple/audio/audio_file.h"
namespace supple::engine {
Result<std::vector<double>> detectTransients(const audio::AudioData&, double sensitivity,
                                             const std::function<bool()>& cancelled = {});
}
