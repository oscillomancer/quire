// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/suite/script_layer.h"

#include <atomic>
#include <string>

namespace supple::suite {

void ScriptLayer::tick() {
    processor().collect();
    reportStatus();
}

void ScriptLayer::redefined() {
    SemanticLayer::redefined();
    statusRestarted();
}

const ScriptDefinition& ScriptLayer::definition() {
    return static_cast<const ScriptDefinition&>(*instance().currentDefinition());
}

ScriptProcessor& ScriptLayer::processor() {
    return *static_cast<ScriptProcessor*>(instance().processor());
}

void ScriptLayer::reportStatus() {
    // " at line N", or nothing when the code cannot know (C++ does not).
    const auto at = [](const std::atomic<uint32_t>& line) {
        const uint32_t n = line.load(std::memory_order_relaxed);
        return n == 0 ? std::string() : " at line " + std::to_string(n);
    };
    const auto& status = processor().instance().status;
    const uint32_t budget = status.budgetExceeded.load(std::memory_order_relaxed);
    if (budget != budgetReported_) {
        budgetReported_ = budget;
        instance().log(CLAP_LOG_WARNING, "the script ran past its loop budget" +
                                             at(status.budgetLine) +
                                             " and was stopped; it is silent until it is "
                                             "reloaded or the host restarts it");
    }
    const uint32_t range = status.outOfRange.load(std::memory_order_relaxed);
    if (range != 0 && outOfRangeReported_ == 0) {
        outOfRangeReported_ = range;
        instance().log(CLAP_LOG_INFO, "the script indexed outside an array" +
                                          at(status.outOfRangeLine) +
                                          " (it read 0 there; a write went nowhere)");
    }
}

}  // namespace supple::suite
