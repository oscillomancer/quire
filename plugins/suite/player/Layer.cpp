// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The player's layer: the suite's script layer (supple/suite/script_layer.h),
// and the script kept current.
//
// Twice a second it looks at the instance's script. Changed and compiling,
// it replaces the running code: in place, crossfaded, when the parameters
// are the same (ScriptProcessor::swap), or by redefining the plugin when
// they are not (the host restarts it and rescans; values carry over by
// key). Either way an open editor is made again, from the new code. A
// script that fails to compile leaves the running one playing, and the
// host's log says why, once per change.
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "Catalog.h"
#include "supple/suite/script_layer.h"

namespace supple::player {
namespace {

class PlayerLayer final : public suite::ScriptLayer {
  public:
    explicit PlayerLayer(plugin::Instance& instance) : ScriptLayer(instance) { remember(); }

    void tick() override {
        ScriptLayer::tick();
        if (++ticks_ % 15 == 0) look();
    }

    void redefined() override {
        ScriptLayer::redefined();
        remember();
    }

  private:
    void remember() {
        source_ = definition().source();
        std::error_code ec;
        if (!definition().file().empty())
            stamp_ = std::filesystem::last_write_time(definition().file(), ec);
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
        if (instance().isActive() && suite::sameShape(*next, definition())) {
            const state::Snapshot s = instance().snapshot();
            ParamValues values(definition().params());
            for (size_t i = 0; i < definition().params().count(); ++i)
                values.setIndex(i, toPlain(definition().params().at(i), s.host[i]));
            processor().swap(next->code(), values);
            statusRestarted();
            instance().rebuildEditor();
            return;
        }
        if (!instance().redefine(next))
            instance().log(CLAP_LOG_WARNING, "the script's new plugin was refused");
    }

    std::string source_;
    std::filesystem::file_time_type stamp_{};
    unsigned ticks_ = 0;
};

}  // namespace
}  // namespace supple::player

std::unique_ptr<supple::plugin::Layer> product::makeLayer(supple::plugin::Instance& instance) {
    return std::make_unique<supple::player::PlayerLayer>(instance);
}
