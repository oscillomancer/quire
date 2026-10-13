// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/suite/script_layer.h"

#include <atomic>
#include <cstring>
#include <string>

#include "dsp/AudioFile.h"
#if SUPPLE_WITH_GUI
#include "supple/suite/script_editor.h"
#endif

namespace supple::suite {

namespace {
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
}  // namespace

const void* ScriptLayer::extension(const char* id) {
    return std::strcmp(id, CLAP_EXT_SUPPLE_PLUGIN_STATUS) == 0 ? &kStatus
                                                               : SemanticLayer::extension(id);
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
    if (s.outOfRange.load(std::memory_order_relaxed))
        problems.push_back(
            problem("script.runtime.index",
                    "An index was outside an array; reads returned zero and writes were discarded.",
                    "audio", "Check the index and array size, then save the script again.",
                    s.outOfRangeLine.load(std::memory_order_relaxed), false));
    for (const auto& p : editorProblems_->asArray()) problems.push_back(p);
    if (!reloadProblem_.isNull()) problems.push_back(reloadProblem_);
    doc["problems"] = std::move(problems);
    return writeAll(out, json::writeCanonical(doc));
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
}

#if SUPPLE_WITH_GUI
std::unique_ptr<gui::EditorBase> ScriptLayer::createEditor(gui::EditorHost& host) {
    // A rebuild after redefinition may precede the layer's redefined() hook.
    // Keep old editors' observations separate from replacement code.
    if (editorProblemsCode_ != processor().editorCode()) {
        editorProblemsCode_ = processor().editorCode();
        editorProblems_ = std::make_shared<json::Value>(json::Value::array());
    }
    return makeScriptEditor(host, processor().editorCode(), processor().editorChannel(),
                            editorProblems_);
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
    const uint32_t range = status.outOfRange.load(std::memory_order_relaxed);
    if (range != 0 && outOfRangeReported_ == 0) {
        instance().log(CLAP_LOG_INFO, "the script indexed outside an array" +
                                          at(status.outOfRangeLine) +
                                          " (it read 0 there; a write went nowhere)");
    }
    outOfRangeReported_ = range;
}

}  // namespace supple::suite
