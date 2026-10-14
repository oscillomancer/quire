// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A generated session for benchmarks and demos (gui.md §9): N channel strips,
// each with an audio item (one of eight generated WAVs) and an 8-bar MIDI
// item, built through the command bus like an agent would.
#pragma once

#include <filesystem>

#include "supple/base/result.h"
#include "supple/cmd/bus.h"

namespace supple::gui {

Status generateSession(cmd::Dispatcher& dispatcher, const std::filesystem::path& mediaDir,
                       int tracks);
// The eight WAVs generateSession uses, written into `mediaDir` unless there
// already (tests generate them once and link them into each session).
Status writeGeneratedMedia(const std::filesystem::path& mediaDir);

}  // namespace supple::gui
