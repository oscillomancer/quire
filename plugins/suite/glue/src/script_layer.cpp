// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/suite/script_layer.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "dsp/AudioFile.h"
#if SUPPLE_WITH_GUI
#include "gui/AutoEditor.h"
#include "supple/suite/route_list.h"
#include "supple/suite/script_editor.h"
#endif

namespace supple::suite {

namespace {
#if SUPPLE_WITH_GUI
// The route list in the automatic editor: five rows.
constexpr float kRouteListWidth = 640.0f, kRouteListHeight = 170.0f;
#endif

json::Value problem(const char* code, std::string message, const char* scope, std::string hint,
                    uint32_t line, bool silent) {
    json::Value p = json::Value::object();
    p["code"] = code;
    p["message"] = std::move(message);
    p["scope"] = scope;
    if (line) p["line"] = static_cast<long long>(line);
    p["hint"] = std::move(hint);
    p["silent"] = silent;
    return p;
}
const clap_plugin_supple_plugin_status_t kStatus = {
    sizeof(clap_plugin_supple_plugin_status_t),
    [](const clap_plugin_t* plugin, const clap_ostream_t* out) {
        auto* layer = dynamic_cast<ScriptLayer*>(plugin::Layer::of(plugin));
        return layer && layer->getStatus(out);
    }};
const clap_plugin_supple_trace_t kTrace = {
    sizeof(clap_plugin_supple_trace_t),
    [](const clap_plugin_t* plugin, uint32_t interval, uint32_t bins) {
        auto* layer = dynamic_cast<ScriptLayer*>(plugin::Layer::of(plugin));
        return layer && layer->startTrace(interval, bins);
    },
    [](const clap_plugin_t* plugin, const clap_ostream_t* out) {
        auto* layer = dynamic_cast<ScriptLayer*>(plugin::Layer::of(plugin));
        return layer && layer->readTrace(out);
    }};

// A trace's bins as numbers, null where nothing was traced.
json::Value series(const std::vector<double>& values, size_t from, int count) {
    json::Value out = json::Value::array();
    for (int b = 0; b < count; ++b) {
        const double v = values[from + static_cast<size_t>(b)];
        out.push_back(std::isnan(v) ? json::Value() : json::Value(v));
    }
    return out;
}
}  // namespace

const void* ScriptLayer::extension(const char* id) {
    if (std::strcmp(id, CLAP_EXT_SUPPLE_PLUGIN_STATUS) == 0) return &kStatus;
    if (std::strcmp(id, CLAP_EXT_SUPPLE_TRACE) == 0) return &kTrace;
    return SemanticLayer::extension(id);
}

bool ScriptLayer::startTrace(uint32_t interval, uint32_t bins) {
    using script::detail::TraceLog;
    if (instance().isActive() || interval == 0 || bins > TraceLog::kMaxBins) return false;
    processor().trace(nullptr);
    trace_ = bins > 0 ? std::make_unique<TraceLog>(
                            static_cast<int>(std::min<uint32_t>(interval, INT32_MAX)),
                            static_cast<int>(bins))
                      : nullptr;
    processor().trace(trace_.get());
    return true;
}

bool ScriptLayer::readTrace(const clap_ostream_t* out) {
    if (instance().isActive() || !trace_) return false;
    const script::detail::TraceLog& log = *trace_;
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.trace/1.0";
    doc["interval"] = static_cast<long long>(log.interval);
    doc["bins"] = static_cast<long long>(log.bins);
    json::Value traces = json::Value::array();
    const int names = log.names.load(std::memory_order_acquire);
    for (int k = 0; k < names; ++k) {
        const size_t from = static_cast<size_t>(k) * static_cast<size_t>(log.bins);
        json::Value t = json::Value::object();
        t["name"] = std::string(log.name[k]);
        t["last"] = series(log.last, from, log.bins);
        t["low"] = series(log.low, from, log.bins);
        t["high"] = series(log.high, from, log.bins);
        t["non_finite"] = static_cast<long long>(log.nonFinite[k].load(std::memory_order_relaxed));
        traces.push_back(std::move(t));
    }
    doc["traces"] = std::move(traces);
    doc["dropped"] = static_cast<long long>(log.dropped.load(std::memory_order_relaxed));
    return writeAll(out, json::writeCanonical(doc));
}

bool ScriptLayer::getStatus(const clap_ostream_t* out) {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.plugin-status/1.0";
    json::Value problems = json::Value::array();
    const auto& s = processor().instance().status;
    if (s.budgetExceeded.load(std::memory_order_relaxed))
        problems.push_back(problem("script.runtime.budget",
                                   "The script exceeded its loop budget and was stopped.", "audio",
                                   "Fix the loop and save the script again, or restart the plugin.",
                                   s.budgetLine.load(std::memory_order_relaxed),
                                   s.silent.load(std::memory_order_relaxed)));
    if (s.memoryRefused.load(std::memory_order_relaxed))
        problems.push_back(problem("script.runtime.memory",
                                   memoryLimitMessage() + " and was stopped.", "prepare",
                                   memoryLimitHint(), 0, true));
    if (s.outOfRange.load(std::memory_order_relaxed))
        problems.push_back(problem(
            "script.runtime.index", "An index was outside an array; it accessed the spare element.",
            "audio", "Check the index and array size, then save the script again.",
            s.outOfRangeLine.load(std::memory_order_relaxed), false));
    for (const auto& p : editorProblems_->asArray()) problems.push_back(p);
    if (!reloadProblem_.isNull()) problems.push_back(reloadProblem_);
    doc["problems"] = std::move(problems);
    return writeAll(out, json::writeCanonical(doc));
}

std::string ScriptLayer::memoryLimitMessage() {
    return "The script's prepare() asked for more than " +
           std::to_string(script::detail::Instance::kMemoryLimit >> 30) + " GiB of memory";
}

std::string ScriptLayer::memoryLimitHint() {
    return "Size its Buffers, DelayLines and Stfts for what the plugin needs (they count "
           "together, up to " +
           std::to_string(script::detail::Instance::kMemoryLimit >> 30) +
           " GiB), then save the script again.";
}

void ScriptLayer::reloadFailed(std::string message, std::string hint, int line) {
    reloadProblem_ = problem("script.reload.failed", std::move(message), "reload", std::move(hint),
                             static_cast<uint32_t>(std::max(0, line)), false);
}

bool ScriptLayer::fileReadable(size_t, const std::string& path, std::string& why) const {
    dsp::AudioReader reader;
    return reader.open(path, &why);
}

void ScriptLayer::tick() {
    processor().collect();
    reportStatus();
}

void ScriptLayer::redefined() {
    SemanticLayer::redefined();
    statusRestarted();
    if (trace_) processor().trace(trace_.get());  // the new definition's processor
}

#if SUPPLE_WITH_GUI
std::unique_ptr<gui::EditorBase> ScriptLayer::createEditor(gui::EditorHost& host) {
    // A rebuild after redefinition may precede the layer's redefined() hook.
    // Keep old editors' observations separate from replacement code.
    if (editorProblemsCode_ != processor().editorCode()) {
        editorProblemsCode_ = processor().editorCode();
        editorProblems_ = std::make_shared<json::Value>(json::Value::array());
    }
    if (auto own = makeScriptEditor(host, processor().editorCode(), processor().editorChannel(),
                                    editorProblems_))
        return own;
    // The automatic editor, with the routes when there are any.
    if (!RouteList::shows(host.pluginDefinition())) return nullptr;
    std::vector<std::unique_ptr<gui::AutoPanel>> panels;
    panels.push_back(
        std::make_unique<RouteList>(host.pluginDefinition(), kRouteListWidth, kRouteListHeight));
    return gui::automaticEditor(host, std::move(panels));
}
#endif

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
    if (budget != 0 && budget != budgetReported_) {
        instance().log(CLAP_LOG_WARNING, "the script ran past its loop budget" +
                                             at(status.budgetLine) +
                                             " and was stopped; it is silent until it is "
                                             "reloaded or the host restarts it");
    }
    budgetReported_ = budget;
    const bool memory = status.memoryRefused.load(std::memory_order_relaxed);
    if (memory && !memoryReported_)
        instance().log(CLAP_LOG_WARNING, memoryLimitMessage() +
                                             "; it is silent until it is reloaded or the host "
                                             "restarts it");
    memoryReported_ = memory;
    const uint32_t range = status.outOfRange.load(std::memory_order_relaxed);
    if (range != 0 && outOfRangeReported_ == 0) {
        instance().log(CLAP_LOG_INFO, "the script indexed outside an array" +
                                          at(status.outOfRangeLine) +
                                          " (it accessed the spare element)");
    }
    outOfRangeReported_ = range;
}

}  // namespace supple::suite
