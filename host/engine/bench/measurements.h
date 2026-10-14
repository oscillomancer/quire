// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <cmath>
#include "supple/engine/bench_probe.h"
#include "supple/json/value.h"

namespace supple::engine::bench {
// Main thread, after the producer has joined. Nearest-rank order statistics.
inline json::Value summarize(const BenchProbe::Series& series, double rate) {
    auto out = json::Value::object();
    std::vector<double> times;
    double sum = 0;
    std::uint64_t frames = 0, over = 0;
    for (const auto& s : series.samples()) {
        times.push_back(s.microseconds);
        sum += s.microseconds;
        frames += s.frames;
        over += s.microseconds > s.frames / rate * 1e6;
    }
    std::sort(times.begin(), times.end());
    auto quantile = [&](double fraction) {
        return times.empty()
                   ? json::Value()
                   : json::Value(
                         times[static_cast<std::size_t>(std::ceil(times.size() * fraction)) - 1]);
    };
    out["samples"] = times.size();
    out["dropped"] = series.dropped;
    out["frames"] = frames;
    out["p50_us"] = quantile(.5);
    out["p99_us"] = quantile(.99);
    out["p999_us"] = quantile(.999);
    out["max_us"] = quantile(1);
    out["mean_us"] = times.empty() ? json::Value() : json::Value(sum / times.size());
    out["load_percent"] = frames ? json::Value(100 * sum / (frames / rate * 1e6)) : json::Value();
    out["over_budget"] = over;
    return out;
}
}  // namespace supple::engine::bench
