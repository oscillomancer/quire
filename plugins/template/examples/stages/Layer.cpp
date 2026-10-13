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
//     stages the instance has, and draws the editor without a window;
//   - a plugin made from a file gets an editor of the layer's, laid out
//     from its stages: a fader each, then the output. The built-in plugin
//     keeps the product's (the automatic editor).
#include <algorithm>
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
#if SUPPLE_WITH_GUI
#include <Theme.h>

#include "gui/EditorBase.h"
#include "gui/Widgets.h"
#endif

namespace product {
namespace {

// The extension, main thread.
struct StagesExtension {
    uint32_t(CLAP_ABI* count)(const clap_plugin_t* plugin);
    // Telemetry while on, as an open editor gets it, so a picture shows
    // what played meanwhile.
    void(CLAP_ABI* watch)(const clap_plugin_t* plugin, bool on);
    // The editor drawn without a window, `scale` times its size: its size,
    // and its pixels (RGBA, rows from the top) when `pixels` holds
    // width * height * 4 bytes. False where it cannot be drawn offscreen.
    bool(CLAP_ABI* picture)(const clap_plugin_t* plugin, double scale, uint32_t* width, uint32_t* height,
                            uint8_t* pixels, size_t capacity);
};

#if SUPPLE_WITH_GUI
// A file's plugin's editor: a fader per stage, its name beneath, then the
// output knob.
class StagesEditor final : public supple::gui::EditorBase {
  public:
    static constexpr float kFader = 64.0f, kMargin = 24.0f, kTop = 56.0f, kHeight = 300.0f;

    StagesEditor(supple::gui::EditorHost& host, const StagesDefinition& d)
        : EditorBase(host, kTheme, widthFor(d), kHeight) {
        const size_t stages = d.stages().size();
        for (size_t k = 0; k < stages; ++k)
            ui().vslider(kStage0 + static_cast<uint32_t>(k),
                         {kMargin + kFader * static_cast<float>(k), kTop, kFader - 16.0f, 200.0f});
        ui().knob(kOutput, {kMargin + kFader * static_cast<float>(stages) + 8.0f, kTop + 50.0f, 92.0f, 100.0f});
    }

    static float widthFor(const StagesDefinition& d) {
        return std::max(320.0f, 2.0f * kMargin + kFader * static_cast<float>(d.stages().size()) + 108.0f);
    }

    void paint(NVGcontext* vg) override {
        const supple::gui::Theme& t = theme();
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, width(), height());
        nvgFillColor(vg, supple::gui::rgb(t.ground));
        nvgFill(vg);
        supple::gui::widgets::caption(vg, kMargin, 30.0f, definition().info().name.c_str(),
                                      supple::gui::rgb(t.textMuted));
        drawControls(vg);
    }
};
#endif

constexpr uint32_t kNamesChunk = supple::state::fourcc("STGN");

class StagesLayer final : public supple::plugin::Layer {
  public:
    explicit StagesLayer(supple::plugin::Instance& instance) : instance_(instance) { look(); }

    const void* extension(const char* id) override {
        static const StagesExtension extension = {
            [](const clap_plugin_t* p) {
                auto* layer = static_cast<StagesLayer*>(Layer::of(p));
                return layer ? static_cast<uint32_t>(layer->definition().stages().size()) : 0u;
            },
            [](const clap_plugin_t* p, bool on) {
                if (auto* layer = static_cast<StagesLayer*>(Layer::of(p))) layer->instance_.watch(on);
            },
            [](const clap_plugin_t* p, double scale, uint32_t* width, uint32_t* height, uint8_t* pixels,
               size_t capacity) {
                auto* layer = static_cast<StagesLayer*>(Layer::of(p));
                supple::Picture picture;
                std::string why;
                if (!layer || !layer->instance_.drawEditor(scale, picture, why)) {
                    if (layer) layer->instance_.log(CLAP_LOG_INFO, "no picture: " + why);
                    return false;
                }
                *width = static_cast<uint32_t>(picture.width);
                *height = static_cast<uint32_t>(picture.height);
                if (pixels && capacity >= picture.rgba.size())
                    std::memcpy(pixels, picture.rgba.data(), picture.rgba.size());
                return true;
            }};
        return std::strcmp(id, "com.suppleaudio.stages/1") == 0 ? &extension : nullptr;
    }

#if SUPPLE_WITH_GUI
    // A file's plugin has the layer's editor; the built-in one, the product's.
    std::unique_ptr<supple::gui::EditorBase> createEditor(supple::gui::EditorHost& host) override {
        if (definition().file().empty()) return nullptr;
        return std::make_unique<StagesEditor>(host, definition());
    }
#endif

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
