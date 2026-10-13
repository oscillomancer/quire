// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What a plugin means, beyond the framework's tables: the documents of the
// com.suppleaudio.semantic-params/1 extension (Quire's
// docs/specs/semantic-params-extension.md, core-params and core-patch).
//
//   - Meaning: each parameter's key ("filter.cutoff"), role tags and choice
//     keys, each property's key, the patch type and a paragraph of docs. A
//     SemanticDefinition carries its own (the player's scripts do); any
//     other Definition gets one derived from its names.
//   - PatchCodec: a state snapshot (core/StateFormat.h) as a patch document
//     and back -- values by key in physical units, choices by key, toggles
//     as booleans, properties as text -- with merge (RFC 7396), validation
//     whose errors carry a JSON Pointer and a hint, the semantic schema
//     document, the patch's JSON Schema, and a description in prose.
//
// Physical units are core-params': a percentage in percent (the framework's
// plain value is a fraction), seconds as milliseconds. A continuous
// parameter's CLAP value is its 0..1 position (normalizedHostValue()).
// Main thread only.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/Definition.h"
#include "core/StateFormat.h"
#include "supple/suite/json.h"

namespace supple::suite {

struct ParamMeaning {
    std::string key;                      // [a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*
    std::vector<std::string> tags;        // "role:level", "stage:filter", "macro"
    std::vector<std::string> choiceKeys;  // a choice's options, lower snake case
};

struct PropertyMeaning {
    std::string key;
};

struct Meaning {
    std::string patchType;                    // "supple.<slug>-patch/<major>.<minor>"
    std::string docs;                         // what the plugin is and how its patch is organised
    std::vector<ParamMeaning> params;         // by parameter table index
    std::vector<PropertyMeaning> properties;  // by property index
};

// A Definition that says what it means.
class SemanticDefinition : public Definition {
  public:
    SemanticDefinition(PluginInfo info, ParamTable params, PropertyList properties, Meaning meaning,
                       std::vector<FactoryPreset> presets = {})
        : Definition(std::move(info), std::move(params), std::move(properties), std::move(presets)),
          meaning_(std::move(meaning)) {}
    const Meaning& meaning() const { return meaning_; }

  private:
    Meaning meaning_;
};

// Lower snake case of any text: "High Cut" -> "high_cut", "Mix %" -> "mix".
std::string snakeCase(std::string_view text);
// A Definition's meaning: its own, or one derived from its names (a key from
// the group and the name, choice keys from the labels, the patch type from
// the plugin's name).
Meaning meaningOf(const Definition& definition);
// What is wrong with a meaning for this definition (keys malformed or not
// unique, counts that do not match); empty when nothing is.
std::string meaningProblems(const Definition& definition, const Meaning& meaning);

// The patch envelope (core-patch §1): what a patch says about itself.
struct Envelope {
    std::optional<std::string> name, author, notes;
    std::vector<std::string> tags;
};

class PatchCodec {
  public:
    // The definition outlives the codec.
    explicit PatchCodec(const Definition& definition);

    const Meaning& meaning() const { return meaning_; }

    // The schema document (supple.semantic-schema/1.0), the "params" member
    // alone (core-params' describeTable), and the patch's JSON Schema.
    json::Value schemaDocument() const;
    json::Value paramsDescription() const;
    json::Value jsonSchema() const;

    // The snapshot as a patch document; with `changedOnly`, only values that
    // differ from their defaults (still a complete patch: missing values
    // read as defaults).
    json::Value patch(const state::Snapshot& snapshot, const Envelope& envelope,
                      bool changedOnly) const;

    // A document onto `snapshot` and `envelope`: replacing everything (what it
    // leaves out takes its default) or merged into what they hold (RFC 7396).
    // Transactional: false with errors, nothing changed, otherwise both
    // updated. Error paths point into the document as merged.
    bool apply(const json::Value& document, bool merge, state::Snapshot& snapshot,
               Envelope& envelope, std::vector<json::Error>& errors) const;

    // A saved patch onto a snapshot being loaded, leniently: values of keys
    // the plugin has replace the snapshot's, anything else is skipped, so a
    // session saved before a parameter went still loads. A snapshot value
    // that writes the patch's own value stays as it is, exactly: a state
    // saved and loaded saves the same bytes again.
    void restore(const json::Value& document, state::Snapshot& snapshot, Envelope& envelope) const;

    // Compact prose: "Tilt: Tilt +3.0 dB; Pivot 1.20 kHz", or "default
    // settings". With `changedOnly`, values that differ from their defaults.
    std::string describe(const state::Snapshot& snapshot, bool changedOnly) const;

    // A parameter's physical value and back (percent in percent, seconds in
    // ms), and its JSON form.
    double physical(size_t index, double plain) const;
    double plain(size_t index, double physical) const;
    json::Value valueJson(size_t index, double host) const;

  private:
    bool readValue(size_t index, const json::Value& v, const std::string& path, double& host,
                   std::vector<json::Error>& errors) const;

    const Definition& definition_;
    Meaning meaning_;
};

// The patch framed as a state chunk (core-patch §4): the magic, version 2,
// the byte length and the canonical JSON.
std::vector<uint8_t> frame(const std::string& canonicalJson, uint32_t magic);
bool unframe(const uint8_t* data, size_t size, uint32_t magic, std::string& json);

}  // namespace supple::suite
