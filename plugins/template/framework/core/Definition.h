// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What a plugin is, as the framework reads it at run time: its identity and
// buses (PluginInfo), its parameters (ParamTable) and its properties
// (PropertyList), together a Definition.
//
// A product written in C++ describes itself at compile time -- product.cmake,
// product/Params.h, product/Properties.h -- and keeps every compile-time
// check on those tables; productDefinition() is that description, made once.
// A run-time product (PRODUCT_RUNTIME ON) has no tables of its own: it
// learns what it is when it runs (a script it loads, say) and builds
// Definitions then, one per plugin its module offers (product/Catalog.h).
// Either way the engine, the CLAP plugin, state, the editor and the tools
// read the Definition an instance was made with, and nothing else.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Layout.h"
#include "core/Param.h"
#include "core/Property.h"

namespace supple {

enum class PluginKind : uint8_t {
    Effect,      // audio in and out
    Instrument,  // notes and MIDI in, audio out
    NoteEffect,  // notes and MIDI in and out, no audio
};

struct BusLayout {
    int channels = 2;
    Layout layout = Layout::Stereo;
    bool operator==(const BusLayout&) const = default;
};

// A plugin's identity and capabilities: what product.cmake says of a C++
// product.
struct PluginInfo {
    std::string id;  // reverse-DNS; permanent once shipped
    std::string name, vendor, url, version, description;
    std::vector<std::string> features;  // CLAP features, the main category first
    std::string stateTag = "Plug";      // the first four bytes of every saved state
    PluginKind kind = PluginKind::Effect;
    std::vector<BusLayout> layouts = {{2, Layout::Stereo}};  // the first is the default; none for a note effect
    bool sidechain = false;
    std::vector<std::string> auxOutputs;  // names of extra outputs
    bool noteInput = false;               // implied by Instrument and NoteEffect
    bool noteOutput = false;              // implied by NoteEffect
    uint32_t noteExpressions = 0;         // bit n: CLAP note expression n
    bool presets = false;
    bool tuning = false;
    int voices = 0;  // a voice pool's size for hosts (CLAP voice-info); 0 leaves it to the processor's kVoices
    bool overlappingNotes = false;

    bool audioInput() const { return kind == PluginKind::Effect; }
    bool audioOutput() const { return kind != PluginKind::NoteEffect; }
    bool instrument() const { return kind == PluginKind::Instrument; }
    bool noteEffect() const { return kind == PluginKind::NoteEffect; }
    bool takesNotes() const { return noteInput || kind != PluginKind::Effect; }
    bool sendsNotes() const { return noteOutput || kind == PluginKind::NoteEffect; }
    int auxCount() const { return static_cast<int>(auxOutputs.size()); }
    // The layout a bus of this many channels has here; Generic when none is declared.
    Layout layoutOf(int channels) const;
    bool offers(int channels) const;
};

// A parameter table: product/Params.h's, or one built at run time. Lookups
// are constant time and allocation-free, for the audio thread; building one
// allocates. Moved, never copied: an owned table's definitions stay where
// they are.
class ParamTable {
  public:
    ParamTable() = default;
    // Over a table in static storage (a C++ product's kParams): not copied.
    ParamTable(const ParamDef* defs, size_t count);
    // Owning its definitions and every string they point to: see Builder.
    explicit ParamTable(std::vector<ParamDef> defs, std::shared_ptr<const void> storage = {});
    ParamTable(ParamTable&&) = default;
    ParamTable& operator=(ParamTable&&) = default;
    ParamTable(const ParamTable&) = delete;
    ParamTable& operator=(const ParamTable&) = delete;

    size_t count() const { return count_; }
    const ParamDef& at(size_t index) const { return defs_[index]; }
    // Table index of the parameter with this ID, or -1.
    int indexOf(uint32_t id) const;
    const ParamDef* find(uint32_t id) const {
        const int i = indexOf(id);
        return i >= 0 ? &defs_[i] : nullptr;
    }
    // The bypass parameter's index, or -1.
    int bypassIndex() const { return bypass_; }
    bool anyRestart() const { return anyRestart_; }
    bool anyPolyMod() const { return anyPolyMod_; }
    // Output parameters, by table index.
    const std::vector<uint16_t>& outputs() const { return outputs_; }
    // Parameters the framework smooths, by table index, and each index's
    // place among them (-1 when not smoothed).
    const std::vector<uint16_t>& smoothed() const { return smoothed_; }
    int smoothedSlot(size_t index) const { return smoothedSlot_[index]; }

    // What is wrong with the table by the rules a C++ product's table is
    // checked against when it compiles; empty when nothing is.
    std::string problems() const;

    // Builds a table at run time: definitions made with the factories in
    // core/Param.h, their text kept here.
    class Builder {
      public:
        // A copy of `text` that lives as long as the table.
        const char* text(std::string_view text);
        // Choice or toggle labels that live as long as the table.
        const char* const* labels(const std::vector<std::string>& labels);
        Builder& add(const ParamDef& def);
        ParamTable build();

      private:
        struct Storage {
            std::deque<std::string> strings;
            std::deque<std::vector<const char*>> lists;
        };
        std::shared_ptr<Storage> storage_ = std::make_shared<Storage>();
        std::vector<ParamDef> defs_;
    };

  private:
    void index();

    const ParamDef* defs_ = nullptr;
    size_t count_ = 0;
    std::vector<ParamDef> owned_;
    std::shared_ptr<const void> storage_;
    // ID -> index: dense for IDs below kDense, sorted pairs above.
    static constexpr uint32_t kDense = 4096;
    std::vector<int32_t> dense_;
    std::vector<std::pair<uint32_t, int32_t>> sparse_;
    int bypass_ = -1;
    bool anyRestart_ = false, anyPolyMod_ = false;
    std::vector<uint16_t> outputs_, smoothed_;
    std::vector<int16_t> smoothedSlot_;
};

// The same for properties (core/Property.h).
class PropertyList {
  public:
    // The longest value any property may declare: state is read whole into
    // memory, and a property is not a place for audio.
    static constexpr uint32_t kMaxBytes = 1u << 20;

    PropertyList() = default;
    PropertyList(const PropertyDef* defs, size_t count);
    explicit PropertyList(std::vector<PropertyDef> defs, std::shared_ptr<const void> storage = {});
    PropertyList(PropertyList&&) = default;
    PropertyList& operator=(PropertyList&&) = default;
    PropertyList(const PropertyList&) = delete;
    PropertyList& operator=(const PropertyList&) = delete;

    size_t count() const { return count_; }
    const PropertyDef& at(size_t index) const { return defs_[index]; }
    int indexOf(uint32_t id) const;
    bool accepts(size_t index, std::string_view value) const { return value.size() <= defs_[index].limit; }
    size_t fileCount() const { return files_; }

    std::string problems() const;

    class Builder {
      public:
        const char* text(std::string_view text);
        Builder& add(const PropertyDef& def);
        PropertyList build();

      private:
        std::shared_ptr<std::deque<std::string>> storage_ = std::make_shared<std::deque<std::string>>();
        std::vector<PropertyDef> defs_;
    };

  private:
    const PropertyDef* defs_ = nullptr;
    size_t count_ = 0;
    std::vector<PropertyDef> owned_;
    std::shared_ptr<const void> storage_;
    size_t files_ = 0;
};

// A factory preset (product/Presets.h): parameter values in plain units, as
// update() reads them, and property values. What it leaves out takes its
// default. Its name is how hosts remember it.
struct FactoryPreset {
    std::string name;
    std::string category;
    std::vector<std::pair<uint32_t, double>> values;
    std::vector<std::pair<uint32_t, std::string>> texts;
};

// One plugin: what it is, its parameters, its properties and its factory
// presets. Immutable once made; instances share it. A run-time product
// derives from it to carry its own (the script a plugin runs).
class Definition {
  public:
    Definition(PluginInfo info, ParamTable params, PropertyList properties = {},
               std::vector<FactoryPreset> presets = {});
    virtual ~Definition() = default;
    Definition(const Definition&) = delete;
    Definition& operator=(const Definition&) = delete;

    const PluginInfo& info() const { return info_; }
    const ParamTable& params() const { return params_; }
    const PropertyList& properties() const { return properties_; }
    const std::vector<FactoryPreset>& presets() const { return presets_; }
    // Index of the factory preset with this name, or -1.
    int findPreset(std::string_view name) const;

    // What is wrong with it, or empty: by the rules a C++ product's tables
    // are checked against when it compiles, and the identity's.
    std::string problems() const;

  private:
    PluginInfo info_;
    ParamTable params_;
    PropertyList properties_;
    std::vector<FactoryPreset> presets_;
};

using DefinitionPtr = std::shared_ptr<const Definition>;

// A product with tables (not PRODUCT_RUNTIME): the one Definition they make,
// built on first use and kept for the life of the process. A run-time
// product has none; calling it fails to link.
const DefinitionPtr& productDefinition();

}  // namespace supple
