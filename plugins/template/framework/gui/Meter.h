// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Display ballistics for a sample-peak meter: instant attack, constant
// fall in dB per second, a peak hold that waits and then falls, and a clip
// latch. Driven by elapsed seconds, never by frame count, so a host that
// repaints at 30 Hz and one that repaints at 120 Hz show the same motion.
#pragma once

#include <algorithm>
#include <cmath>

namespace supple::gui {

class PeakMeter {
  public:
    static constexpr float kFloorDb = -72.0f;
    static constexpr float kFallDbPerSecond = 26.0f;
    static constexpr float kHoldSeconds = 1.2f;
    static constexpr float kHoldFallDbPerSecond = 24.0f;
    static constexpr float kClipSeconds = 2.0f;

    // One telemetry frame's linear sample peak.
    void push(float linearPeak) {
        pending_ = std::max(pending_, linearPeak);
        if (linearPeak >= 1.0f) {
            clipped_ = true;
            clipAge_ = 0.0f;
        }
    }

    void advance(double seconds) {
        const float dt = static_cast<float>(std::clamp(seconds, 0.0, 0.25));
        const float target = pending_ > 0.0f ? std::max(kFloorDb, 20.0f * std::log10(pending_)) : kFloorDb;
        pending_ = 0.0f;
        level_ = std::max(target, std::max(kFloorDb, level_ - kFallDbPerSecond * dt));
        if (target >= hold_) {
            hold_ = target;
            holdAge_ = 0.0f;
        } else if ((holdAge_ += dt) > kHoldSeconds) {
            hold_ = std::max(kFloorDb, hold_ - kHoldFallDbPerSecond * dt);
        }
        if (clipped_ && (clipAge_ += dt) > kClipSeconds) clipped_ = false;
    }

    void reset() { *this = PeakMeter{}; }

    float levelDb() const { return level_; }
    float holdDb() const { return hold_; }
    bool clipped() const { return clipped_; }
    // Still moving on screen, so the editor must keep repainting.
    bool moving() const { return level_ > kFloorDb || hold_ > kFloorDb || clipped_; }

  private:
    float pending_ = 0.0f;
    float level_ = kFloorDb;
    float hold_ = kFloorDb;
    float holdAge_ = 0.0f;
    float clipAge_ = 0.0f;
    bool clipped_ = false;
};

}  // namespace supple::gui
