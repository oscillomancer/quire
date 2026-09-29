// The semantic-params extension and CLAP state for plugins whose patch has
// structure (slots, routes, types) rather than a flat parameter table.
//
// The plugin implements SemanticModel over its own patch type; SemanticAdapter
// turns it into the com.suppleaudio.semantic-params/1 extension and the CLAP
// state stream, with the same wire formats as FlatSemantic (schema document,
// canonical patch JSON, JSON error arrays, framed state). Main thread only.
#pragma once

#include <clap/clap.h>

#include <cstdint>
#include <string>

#include "supple/base/result.h"
#include "supple/clap/semantic_params.h"
#include "supple/json/json.h"
#include "supple/params/params.h"

namespace supple::clapx {

class SemanticModel {
  public:
    virtual ~SemanticModel() = default;
    virtual params::Table table() const = 0;
    virtual std::string patchTypeId() const = 0;                // "supple.orrery-patch/1.0"
    virtual json::Value patchSchema() const = 0;                // JSON Schema of the patch document
    virtual std::string patchJson(bool changedOnly) const = 0;  // canonical
    // REPLACE (a complete patch), MERGE (RFC 7396) or VALIDATE_ONLY (as MERGE,
    // changes nothing). Transactional: on error nothing changes; error paths
    // point into the submitted (or merged) document.
    virtual Status apply(std::uint32_t mode, const json::Value& document) = 0;
    virtual std::string describe(bool changedOnly) const = 0;  // ≤ 600 chars when changedOnly
    // False for parameters the current structure does not use (a slot that is
    // absent); the schema marks them "active": false.
    virtual bool paramActive(std::size_t /*index*/) const { return true; }
};

class SemanticAdapter {
  public:
    struct Identity {
        std::string id, name, version, docs;
        std::uint32_t stateMagic = 0;  // permanent 4-byte state tag
    };
    SemanticAdapter(SemanticModel& model, Identity identity);

    bool getSchema(const clap_ostream_t* out) const;
    bool getPatch(std::uint32_t detail, const clap_ostream_t* out) const;
    bool setPatch(std::uint32_t mode, const char* json, std::uint32_t size,
                  const clap_ostream_t* errors);
    bool describe(std::uint32_t detail, const clap_ostream_t* out) const;
    bool saveState(const clap_ostream_t* out) const;
    bool loadState(const clap_istream_t* in);  // REPLACE semantics

    using Resolver = SemanticAdapter* (*)(const clap_plugin_t*);
    template <Resolver R>
    static const clap_plugin_supple_semantic_params_t* extension() {
        static const clap_plugin_supple_semantic_params_t ext = {
            sizeof(clap_plugin_supple_semantic_params_t),
            [](const clap_plugin_t* p, const clap_ostream_t* o) { return R(p)->getSchema(o); },
            [](const clap_plugin_t* p, uint32_t d, const clap_ostream_t* o) {
                return R(p)->getPatch(d, o);
            },
            [](const clap_plugin_t* p, uint32_t m, const char* j, uint32_t n,
               const clap_ostream_t* e) { return R(p)->setPatch(m, j, n, e); },
            [](const clap_plugin_t* p, uint32_t d, const clap_ostream_t* o) {
                return R(p)->describe(d, o);
            },
            nullptr};
        return &ext;
    }

  private:
    SemanticModel& model_;
    Identity id_;
};

}  // namespace supple::clapx
