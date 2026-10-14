// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Patch and preset documents (spec: docs/specs/core-patch.md, ADR-0009).
//
// A patch is a plugin's complete, human-readable state: the primary interface
// for agents, the preset file format and the CLAP state. Documents are
// canonical JSON:
//
//   {"schema": "supple.<plugin>-patch/<major>.<minor>",
//    "name": …, "author": …, "tags": [...], "notes": …, "seed": …,   (envelope, all optional)
//    …plugin body members…}
//
// PatchType<Body> handles structured bodies described by a core-schema
// Schema<Body>; FlatPatchType handles plugins whose body is just their
// parameter table ("params": {"<key>": value}). Both share the version
// policy (ADR-0009), migrations, the envelope, transactional merge (RFC 7396)
// and diff. Main-thread only.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/json.h"
#include "supple/params/params.h"
#include "supple/schema/document.h"
#include "supple/schema/schema.h"

namespace supple::patch {

using schema::Migration;
using schema::Warning;

struct Envelope {
    std::optional<std::string> name, author, notes;
    std::vector<std::string> tags;
    std::optional<std::uint32_t> seed;
    friend bool operator==(const Envelope&, const Envelope&) = default;
};

template <class Body>
struct Patch {
    Envelope meta;
    Body body;
};

// Shared machinery: document ID and version policy, migrations, envelope.
class Codec {
  public:
    // plugin "eq" -> type "eq-patch" -> "supple.eq-patch/<major>.<minor>".
    // migrations[k - 1] upgrades a whole document from major k to k + 1.
    Codec(std::string plugin, int major, int minor, bool hasSeed,
          std::vector<Migration> migrations);

    const schema::DocumentId& id() const { return id_; }
    const std::string& schemaId() const { return schemaId_; }
    bool hasSeed() const { return hasSeed_; }

    struct Split {
        Envelope meta;
        json::Value body;               // the document without "schema" and envelope members
        schema::DecodeOptions options;  // Ignore unknown members for newer minors
    };
    // Checks the ID and version, migrates older majors, validates and removes
    // the envelope.
    Result<Split> split(const json::Value& document, std::vector<Warning>* warnings) const;
    // {"schema", envelope members, body members} in canonical order.
    json::Value join(const Envelope& meta, const json::Value& body) const;
    // JSON Schema properties for "schema" and the envelope members.
    json::Value envelopeProperties() const;
    // Rejects a merge patch whose "schema" member names another type.
    Status checkMergePatch(const json::Value& mergePatch) const;

  private:
    schema::DocumentId id_;
    std::string schemaId_;
    bool hasSeed_;
    std::vector<Migration> migrations_;
};

template <class Body>
class PatchType {
  public:
    PatchType(std::string plugin, int major, int minor, schema::Schema<Body> body, bool hasSeed,
              std::vector<Migration> migrations = {})
        : codec_(std::move(plugin), major, minor, hasSeed, std::move(migrations)),
          body_(std::move(body)) {}

    Result<Patch<Body>> fromJson(const json::Value& document,
                                 std::vector<Warning>* warnings = nullptr) const {
        auto split = codec_.split(document, warnings);
        if (!split) return std::move(split).error();
        auto body = body_.decode(split->body, split->options, warnings);
        if (!body) return std::move(body).error();
        return Patch<Body>{std::move(split->meta), std::move(*body)};
    }

    Result<Patch<Body>> parse(std::string_view text,
                              std::vector<Warning>* warnings = nullptr) const {
        auto value = json::parse(text);
        if (!value) return std::move(value).error();
        return fromJson(*value, warnings);
    }

    json::Value toJson(const Patch<Body>& p) const {
        return codec_.join(p.meta, body_.encode(p.body));
    }
    std::string serialize(const Patch<Body>& p) const { return json::writeCanonical(toJson(p)); }

    // RFC 7396 merge, then full validation; on error nothing changes and the
    // errors' paths refer to the merged document.
    Result<Patch<Body>> merge(const Patch<Body>& current, const json::Value& mergePatch) const {
        SUPPLE_TRY(codec_.checkMergePatch(mergePatch));
        return fromJson(json::mergePatch(toJson(current), mergePatch));
    }

    // Minimal merge patch turning a into b.
    json::Value diff(const Patch<Body>& a, const Patch<Body>& b) const {
        auto p = json::createMergePatch(toJson(a), toJson(b));
        return p ? std::move(*p) : json::Value::object();  // documents never hold null members
    }

    json::Value jsonSchema() const {
        json::Value s = body_.jsonSchema();
        json::Value properties = codec_.envelopeProperties();
        if (const auto* bodyProps = s.find("properties"); bodyProps && bodyProps->isObject())
            for (const auto& m : bodyProps->asObject()) properties[m.key] = m.value;
        s["properties"] = std::move(properties);
        json::Value required = json::Value::array();
        required.push_back("schema");
        if (const auto* r = s.find("required"); r && r->isArray())
            for (const auto& v : r->asArray()) required.push_back(v);
        s["required"] = std::move(required);
        return s;
    }

    const std::string& schemaId() const { return codec_.schemaId(); }
    const Codec& codec() const { return codec_; }

  private:
    Codec codec_;
    schema::Schema<Body> body_;
};

// --- flat-parameter patches --------------------------------------------------

struct FlatBody {
    std::vector<double> values;  // physical values, indexed like the table
    friend bool operator==(const FlatBody&, const FlatBody&) = default;
};

enum class Detail { Full, Changed };

class FlatPatchType {
  public:
    FlatPatchType(std::string plugin, int major, int minor, params::Table table, bool hasSeed,
                  std::vector<Migration> migrations = {});

    Patch<FlatBody> defaults() const;
    Result<Patch<FlatBody>> fromJson(const json::Value& document,
                                     std::vector<Warning>* warnings = nullptr) const;
    Result<Patch<FlatBody>> parse(std::string_view text,
                                  std::vector<Warning>* warnings = nullptr) const;
    json::Value toJson(const Patch<FlatBody>& p, Detail detail = Detail::Full) const;
    std::string serialize(const Patch<FlatBody>& p, Detail detail = Detail::Full) const;
    Result<Patch<FlatBody>> merge(const Patch<FlatBody>& current,
                                  const json::Value& mergePatch) const;
    json::Value diff(const Patch<FlatBody>& a, const Patch<FlatBody>& b) const;
    json::Value jsonSchema() const;
    const std::string& schemaId() const { return codec_.schemaId(); }
    params::Table table() const { return table_; }
    const Codec& codec() const { return codec_; }

  private:
    Codec codec_;
    params::Table table_;
};

// --- plugin state framing (CLAP state stream) ------------------------------------

inline constexpr std::uint32_t kStateFormatVersion = 2;  // 1 = the template's binary id/value form
inline constexpr std::size_t kMaxStateBytes = 16u << 20;

// magic (LE u32) | version 2 (LE u32) | byte length (LE u32) | UTF-8 JSON.
// A document over kMaxStateBytes is an error (patch.state.too_large).
Result<std::vector<std::uint8_t>> encodeState(std::string_view canonicalJson, std::uint32_t magic);
// True when `bytes` start with this plugin's magic and format version 2, i.e.
// the stream claims to be the framed form. Plugins with a legacy loader use
// it to decide: a claimed-but-malformed frame is corrupt, never legacy.
bool isFramedState(std::span<const std::uint8_t> bytes, std::uint32_t magic);
// Returns the JSON text. Truncated, oversized, wrong magic/version or
// non-UTF-8 input is an error (patch.state.*). Nothing is applied here.
Result<std::string> decodeState(std::span<const std::uint8_t> bytes, std::uint32_t magic);

// --- descriptions ------------------------------------------------------------------

// "EQ: band1 High-pass 90.0 Hz; band2 Bell 250 Hz -3.0 dB" — changed params
// grouped by the first key segment, in table order.
std::string describeFlat(const FlatPatchType& type, const Patch<FlatBody>& p,
                         Detail detail = Detail::Changed);
std::string quantity(double v, units::Unit u, int digits = 3);

}  // namespace supple::patch
