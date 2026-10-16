// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>

namespace supple::gui {

// Average the intervals between the last eight taps. Timestamps are supplied
// by the caller, so this has no clock or window dependency.
class TapTempo {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    std::optional<double> tap(TimePoint now) {
        if (count_ &&
            (now <= taps_[count_ - 1] || now - taps_[count_ - 1] > std::chrono::seconds(2)))
            count_ = 0;
        if (count_ == taps_.size()) {
            for (std::size_t i = 1; i < count_; ++i) taps_[i - 1] = taps_[i];
            --count_;
        }
        taps_[count_++] = now;
        if (count_ < 2) return {};
        const double seconds = std::chrono::duration<double>(now - taps_[0]).count();
        return std::clamp(60.0 * static_cast<double>(count_ - 1) / seconds, 1.0, 1000.0);
    }

  private:
    std::array<TimePoint, 8> taps_{};
    std::size_t count_ = 0;
};

}  // namespace supple::gui
