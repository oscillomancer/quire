// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The player's layer (plugin/Layer.h): the semantic-params extension
// (supple/suite/semantic_layer.h), and the script kept current.
//
// Twice a second it looks at the instance's script. Changed and compiling,
// it replaces the running code: in place, crossfaded, when the parameters
// are the same (ScriptProcessor::swap), or by redefining the plugin when
// they are not (the host restarts it and rescans; values carry over by
// key). A script that fails to compile leaves the running one playing, and
// the host's log says why, once per change. So does a script stopped for
// spending its budget, with the line it was on.
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "Catalog.h"
#include "Processor.h"
#include "supple/suite/semantic_layer.h"

namespace supple::player {
namespace {

class PlayerLayer final : public suite::SemanticLayer {
  public:
    explicit PlayerLayer(plugin::Instance& instance) : SemanticLayer(instance) { remember(); }

    void tick() override {
        processor().collect();
        reportStatus();
        if (++ticks_ % 15 == 0) look();
    }

    void redefined() override {
        SemanticLayer::redefined();
        remember();
    }

  private:
    const ScriptDefinition& definition() {
        return static_cast<const ScriptDefinition&>(*instance().currentDefinition());
    }
    ScriptProcessor& processor() { return *static_cast<ScriptProcessor*>(instance().processor()); }

    void remember() {
        source_ = definition().source();
        std::error_code ec;
        if (!definition().file().empty())
            stamp_ = std::filesystem::last_write_time(definition().file(), ec);
        budgetReported_ = outOfRangeReported_ = 0;
    }

    void look() {
        const std::filesystem::path& file = definition().file();
        if (file.empty()) return;  // embedded: nothing to follow
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(file, ec);
        if (ec || stamp == stamp_) return;
        stamp_ = stamp;
        std::string source;
        if (!readScript(file, source) || source == source_) return;
        std::string problems;
        const auto next = compileScript(file, source, &problems);
        if (!next) {
            instance().log(CLAP_LOG_WARNING, "the script was not reloaded:\n" + problems);
            return;
        }
        source_ = source;
        if (next->info().id != definition().info().id) {
            instance().log(CLAP_LOG_WARNING,
                           "the script's ID changed; it becomes another plugin when the host "
                           "lists plugins again, and this instance keeps the old one");
            return;
        }
        if (instance().isActive() && sameShape(*next, definition())) {
            const state::Snapshot s = instance().snapshot();
            ParamValues values(definition().params());
            for (size_t i = 0; i < definition().params().count(); ++i)
                values.setIndex(i, toPlain(definition().params().at(i), s.host[i]));
            processor().swap(next->program(), values);
            budgetReported_ = outOfRangeReported_ = 0;
            return;
        }
        if (!instance().redefine(next))
            instance().log(CLAP_LOG_WARNING, "the script's new plugin was refused");
    }

    void reportStatus() {
        const auto& status = processor().instance().status;
        const uint32_t budget = status.budgetExceeded.load(std::memory_order_relaxed);
        if (budget != budgetReported_) {
            budgetReported_ = budget;
            instance().log(CLAP_LOG_WARNING,
                           "the script ran past its loop budget at line " +
                               std::to_string(status.budgetLine.load(std::memory_order_relaxed)) +
                               " and was stopped; it is silent until it is reloaded or the host "
                               "restarts it");
        }
        const uint32_t range = status.outOfRange.load(std::memory_order_relaxed);
        if (range != 0 && outOfRangeReported_ == 0) {
            outOfRangeReported_ = range;
            instance().log(
                CLAP_LOG_INFO,
                "the script indexed outside an array at line " +
                    std::to_string(status.outOfRangeLine.load(std::memory_order_relaxed)) +
                    " (it read 0 there; a write went nowhere)");
        }
    }

    std::string source_;
    std::filesystem::file_time_type stamp_{};
    uint32_t budgetReported_ = 0, outOfRangeReported_ = 0;
    unsigned ticks_ = 0;
};

}  // namespace
}  // namespace supple::player

std::unique_ptr<supple::plugin::Layer> product::makeLayer(supple::plugin::Instance& instance) {
    return std::make_unique<supple::player::PlayerLayer>(instance);
}
