// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <utility>

#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"
#include "tap_tempo.h"

namespace supple::gui {

class TapTempoButton : public ui::Button {
  public:
    TapTempoButton(std::string id, ui::TextField& field, ui::Root& root)
        : ui::Button(std::move(id), "Tap") {
        onClick = [this, &field, &root] {
            if (const auto bpm = taps_.tap(now())) {
                char text[24];
                std::snprintf(text, sizeof text, "%.1f", *bpm);
                std::string value(text);
                if (value.ends_with(".0")) value.resize(value.size() - 2);
                field.setText(std::move(value));
            }
            // Enter should still submit the tempo after clicking Tap.
            root.focus(&field);
        };
    }

    // PointerEvent has no timestamp. Read a monotonic clock on activation;
    // callers may supply a deterministic clock when driving the UI in tests.
    std::function<TapTempo::TimePoint()> now = TapTempo::Clock::now;

  private:
    TapTempo taps_;
};

}  // namespace supple::gui
