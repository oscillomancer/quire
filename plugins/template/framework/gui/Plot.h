// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The arithmetic of graphs: an axis maps values to design units, linear or
// logarithmic, and a ValueHistory keeps a scrolling trace (gain reduction
// over the last few seconds). Widgets.h draws grids, curves and traces with
// them.
//
//   const Axis hz = Axis::logarithmic(20.0, 20000.0, graph.x, graph.right());
//   const Axis db = Axis::linear(-48.0, 12.0, graph.bottom(), graph.y);  // up is louder
//   widgets::frequencyGrid(vg, theme(), graph, hz);
//   widgets::levelGrid(vg, theme(), graph, db, 12.0);
//   widgets::plot(vg, graph, hz, db, [&](double f) { return responseDb(f); }, rgb(theme().accent));
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace supple::gui {

struct Axis {
    double lo = 0.0, hi = 1.0;   // the values at the two ends
    float from = 0.0f, to = 1.0f;  // where they are drawn
    bool log = false;

    static Axis linear(double lo, double hi, float from, float to) { return {lo, hi, from, to, false}; }
    // lo and hi must both be positive.
    static Axis logarithmic(double lo, double hi, float from, float to) { return {lo, hi, from, to, true}; }

    // Where a value is drawn; outside [lo, hi] lands outside [from, to].
    float at(double v) const {
        const double t = log ? std::log(std::max(v, 1e-30) / lo) / std::log(hi / lo) : (v - lo) / (hi - lo);
        return from + (to - from) * static_cast<float>(t);
    }
    // The value drawn at a point.
    double value(float p) const {
        const double t = (p - from) / (to - from);
        return log ? lo * std::pow(hi / lo, t) : lo + (hi - lo) * t;
    }
    // at(), held to the axis's ends.
    float clamped(double v) const { return at(std::clamp(v, std::min(lo, hi), std::max(lo, hi))); }
};

// The last `capacity` values pushed, for a trace that scrolls.
class ValueHistory {
  public:
    explicit ValueHistory(size_t capacity = 240, float initial = 0.0f) : values_(std::max<size_t>(capacity, 2), initial) {}

    void push(float v) {
        values_[next_] = v;
        next_ = (next_ + 1) % values_.size();
    }
    void fill(float v) { std::fill(values_.begin(), values_.end(), v); }
    size_t size() const { return values_.size(); }
    // age 0 is the newest.
    float at(size_t age) const {
        const size_t n = values_.size();
        return values_[(next_ + n - 1 - std::min(age, n - 1)) % n];
    }

  private:
    std::vector<float> values_;
    size_t next_ = 0;
};

}  // namespace supple::gui
