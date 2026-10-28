// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The layer of a plugin made from a script (plugin/Layer.h): the
// semantic-params extension (semantic_layer.h), the code the processor let
// go of freed on the main thread, what the script's status says in the
// host's log -- once per occurrence, with the line it was on: a loop that
// spent its budget and was stopped, an index outside an array -- and the
// editor of a script that draws its own (script_editor.h; the product's,
// the automatic one, otherwise, with a list of the modulation routes when
// the script has any: route_list.h), and what the script's trace() calls
// record for a host that traces it (com.suppleaudio.trace/1). The player's
// layer derives from it to follow the script's file. Main thread only.
#pragma once

#include <cstdint>
#include <memory>

#include "supple/clap/plugin_status.h"
#include "supple/clap/trace.h"
#include "supple/suite/script_definition.h"
#include "supple/suite/script_processor.h"
#include "supple/suite/semantic_layer.h"

namespace supple::suite {

class ScriptLayer : public SemanticLayer {
  public:
    explicit ScriptLayer(plugin::Instance& instance) : SemanticLayer(instance) {}

    void tick() override;
    void redefined() override;
    const void* extension(const char* id) override;
    bool getStatus(const clap_ostream_t* out);
    // com.suppleaudio.trace/1's start and read.
    bool startTrace(uint32_t interval, uint32_t bins);
    bool readTrace(const clap_ostream_t* out);
#if SUPPLE_WITH_GUI
    std::unique_ptr<gui::EditorBase> createEditor(gui::EditorHost& host) override;
#endif

  protected:
    // A file member's file decodes as audio.
    bool fileReadable(size_t index, const std::string& path, std::string& why) const override;
    const ScriptDefinition& definition();
    ScriptProcessor& processor();
    // The status is for code now running: what was reported is forgotten.
    void statusRestarted() {
        budgetReported_ = outOfRangeReported_ = 0;
        memoryReported_ = false;
        reloadResolved();
    }
    void reloadResolved() { reloadProblem_ = json::Value(); }
    void reloadFailed(std::string message, std::string hint, int line = 0);
    // What a script refused memory in prepare() is told, and how to fix it.
    static std::string memoryLimitMessage();
    static std::string memoryLimitHint();

  private:
    void reportStatus();

    uint32_t budgetReported_ = 0, outOfRangeReported_ = 0;
    bool memoryReported_ = false;
    json::Value reloadProblem_;
    std::shared_ptr<json::Value> editorProblems_ =
        std::make_shared<json::Value>(json::Value::array());
    std::shared_ptr<const script::detail::Code> editorProblemsCode_;
    std::unique_ptr<script::detail::TraceLog> trace_;
};

}  // namespace supple::suite
