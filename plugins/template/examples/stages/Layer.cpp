// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Stages' layer (plugin/Layer.h), one per plugin instance:
//
//   - a plugin made from a file is remade when the file changes: the layer
//     looks twice a second, and redefines the instance with what the file
//     says now. The host restarts it and rescans its parameters;
//   - every stage's value is kept by name in the state (chunk "STGN"), so a
//     session, or a redefinition, finds each stage's value where the stage
//     moved to, not where its parameter ID points now;
//   - a CLAP extension of its own, com.suppleaudio.stages/1, says how many
//     stages the instance has.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "Catalog.h"
#include "plugin/Layer.h"

namespace product {
namespace {

// The extension: one call, main thread.
struct StagesExtension {
    uint32_t(CLAP_ABI* count)(const clap_plugin_t* plugin);
};

constexpr uint32_t kNamesChunk = supple::state::fourcc("STGN");

class StagesLayer final : public supple::plugin::Layer {
  public:
    explicit StagesLayer(supple::plugin::Instance& instance) : instance_(instance) { look(); }

    const void* extension(const char* id) override {
        static const StagesExtension extension = {[](const clap_plugin_t* p) {
            auto* layer = static_cast<StagesLayer*>(Layer::of(p));
            return layer ? static_cast<uint32_t>(layer->definition().stages().size()) : 0u;
        }};
        return std::strcmp(id, "com.suppleaudio.stages/1") == 0 ? &extension : nullptr;
    }

    // "name\tvalue\n" for each stage, the host value written so that it
    // reads back exactly. It names every stage the state has: one it does
    // not name is new, and starts at its default.
    uint32_t chunkTag() const override { return kNamesChunk; }
    std::vector<uint8_t> saveChunk(const supple::state::Snapshot& s, supple::state::Context) override {
        const auto& d = static_cast<const StagesDefinition&>(*s.definition);
        std::string text;
        for (size_t k = 0; k < d.stages().size(); ++k) {
            const int i = d.params().indexOf(kStage0 + static_cast<uint32_t>(k));
            char value[32];
            std::snprintf(value, sizeof(value), "%.17g", s.host[static_cast<size_t>(i)]);
            text += d.stages()[k] + "\t" + value + "\n";
        }
        return {text.begin(), text.end()};
    }
    bool loadChunk(const uint8_t* data, size_t size, supple::state::Snapshot& s, supple::state::Context) override {
        const auto& d = static_cast<const StagesDefinition&>(*s.definition);
        const std::string text(reinterpret_cast<const char*>(data), size);
        for (size_t k = 0; k < d.stages().size(); ++k) {
            const int i = d.params().indexOf(kStage0 + static_cast<uint32_t>(k));
            s.host[static_cast<size_t>(i)] = supple::hostDefault(d.params().at(static_cast<size_t>(i)));
        }
        for (size_t start = 0; start < text.size();) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(start, end - start);
            start = end + 1;
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) return false;  // not ours: refuse the state
            const std::string name = line.substr(0, tab);
            for (size_t k = 0; k < d.stages().size(); ++k) {
                if (d.stages()[k] != name) continue;
                const int i = d.params().indexOf(kStage0 + static_cast<uint32_t>(k));
                const supple::ParamDef& p = d.params().at(static_cast<size_t>(i));
                s.host[static_cast<size_t>(i)] = supple::sanitizeHost(p, std::strtod(line.c_str() + tab + 1, nullptr));
            }
        }
        return true;
    }

    void tick() override {
        if (++ticks_ % 15 == 0) look();  // twice a second
    }

    void redefined() override { look(); }

  private:
    const StagesDefinition& definition() const {
        return static_cast<const StagesDefinition&>(*instance_.currentDefinition());
    }

    // Remakes the plugin when its file says something new.
    void look() {
        const StagesDefinition& d = definition();
        if (d.file().empty()) return;
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(d.file(), ec);
        if (ec || stamp == stamp_) return;
        stamp_ = stamp;
        std::vector<std::string> stages = readStages(d.file());
        if (stages.empty() || stages == d.stages()) return;
        if (!instance_.redefine(makeStages(d.info().id, d.info().name, std::move(stages), d.file())))
            instance_.log(CLAP_LOG_WARNING, "the stages file names a stage twice, or one called Output");
    }

    supple::plugin::Instance& instance_;
    std::filesystem::file_time_type stamp_{};
    unsigned ticks_ = 0;
};

}  // namespace

std::unique_ptr<supple::plugin::Layer> makeLayer(supple::plugin::Instance& instance) {
    return std::make_unique<StagesLayer>(instance);
}

}  // namespace product
