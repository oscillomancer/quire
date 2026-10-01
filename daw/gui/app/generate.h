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

}  // namespace supple::gui
