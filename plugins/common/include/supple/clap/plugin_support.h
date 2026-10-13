// Plugin-side support for Quire's stock plugins (ADR-0010, ADR-0011):
// everything a template-derived plugin needs to expose a core-params table
// through CLAP params, a flat core-patch document through CLAP state, and the
// com.suppleaudio.semantic-params/1 extension — so the plugin's own wrapper
// code shrinks to one-line forwards.
//
//   ParamStore     physical parameter values shared by the main and audio
//                  threads (wait-free atomics + a change generation).
//   ClapParams     clap_plugin_params_t helpers (info, text) over a table.
//   FlatSemantic   the extension + JSON state for a FlatPatchType plugin.
//
// Threading: ParamStore reads/writes and generation() are RT-safe; everything
// else is main-thread (CLAP's rules for params info/text, state and the
// extension). Whole patches (set_patch, state load) reach the audio thread
// atomically through readAll(): see ParamStore.
#pragma once

#include <clap/clap.h>

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "supple/base/rt.h"
#include "supple/clap/semantic_params.h"
#include "supple/params/params.h"
#include "supple/patch/patch.h"

namespace supple::clapx {

// --- ParamStore --------------------------------------------------------------------

class ParamStore {
  public:
    explicit ParamStore(params::Table table);

    params::Table table() const { return table_; }
    std::size_t size() const { return table_.size(); }

    // Physical values (Hz, ms, dB, enum index, 0/1). Clamped on write.
    double get(std::size_t index) const SUPPLE_RT;
    void set(std::size_t index, double physical) SUPPLE_RT;
    // CLAP-domain access (ADR-0010) by CLAP id; false for unknown ids.
    bool getClap(clap_id id, double& clapValue) const SUPPLE_RT;
    bool setClap(clap_id id, double clapValue) SUPPLE_RT;

    // Monotonic counter bumped by every set()/assign(); the audio thread
    // compares it with the value it last consumed to know when to push values
    // into its DSP.
    std::uint64_t generation() const SUPPLE_RT {
        return generation_.load(std::memory_order_acquire);
    }

    // Audio thread: copies every value (out.size() == size()) as one
    // consistent set with respect to assign(). Returns false while an assign()
    // is in progress; the caller keeps its previous values and tries again next
    // block (wait-free, never spins). Single-value set() calls are not grouped.
    //
    //   if (store.generation() != seen && store.readAll(values)) {
    //       seen = generation-read-before-readAll; push values into the DSP;
    //   }
    bool readAll(std::span<double> out) const SUPPLE_RT;

    void resetToDefaults();                    // main thread; atomic like assign()
    patch::FlatBody snapshot() const;          // main thread
    void assign(const patch::FlatBody& body);  // main thread; one seqlock write + one bump

  private:
    params::Table table_;
    std::unique_ptr<std::atomic<double>[]> values_;
    std::atomic<std::uint64_t> generation_{1};
    std::atomic<std::uint64_t> seq_{0};  // odd while assign() writes (seqlock)
};

// --- CLAP params helpers -----------------------------------------------------------

struct ClapParams {
    static std::uint32_t count(params::Table t) { return static_cast<std::uint32_t>(t.size()); }
    static bool info(params::Table t, std::uint32_t index, clap_param_info_t* out);
    static bool valueToText(params::Table t, clap_id id, double clapValue, char* out,
                            std::uint32_t size);
    static bool textToValue(params::Table t, clap_id id, const char* text, double* clapValue);
    // Applies every CLAP_EVENT_PARAM_VALUE in `in` to the store (flush and
    // process); returns true if any value changed.
    static bool applyEvents(ParamStore& store, const clap_input_events_t* in) SUPPLE_RT;
};

// --- FlatSemantic -----------------------------------------------------------------

class FlatSemantic {
  public:
    // `describePrefix` starts describe() output ("EQ: "). `stateMagic` is the
    // plugin's permanent 4-byte state tag.
    FlatSemantic(const patch::FlatPatchType& type, ParamStore& store, std::string pluginId,
                 std::string docs, std::string describePrefix, std::uint32_t stateMagic);

    // Extension entry points (main thread). Plugins forward to these.
    bool getSchema(const clap_ostream_t* out) const;
    bool getPatch(std::uint32_t detail, const clap_ostream_t* out) const;
    bool setPatch(std::uint32_t mode, const char* json, std::uint32_t size,
                  const clap_ostream_t* errors);
    bool describe(std::uint32_t detail, const clap_ostream_t* out) const;
    // Set before exposing extension<Resolver, true>(). Non-telemetry plugins
    // expose a null member; changing the hook never changes RT state.
    std::function<bool(const clap_ostream_t*)> telemetry;
    bool getTelemetry(const clap_ostream_t* out) const { return telemetry && telemetry(out); }

    // CLAP state: framed canonical patch JSON; load is transactional (parse and
    // validate fully, then apply). `legacyLoad`, when set, is tried for a stream
    // that is not in the framed form (e.g. the template's binary state v1).
    bool saveState(const clap_ostream_t* out) const;
    bool loadState(const clap_istream_t* in);
    std::function<bool(std::span<const std::uint8_t>, ParamStore&)> legacyLoad;

    // The current patch (values from the store plus the stored envelope).
    patch::Patch<patch::FlatBody> current() const;
    const patch::Envelope& envelope() const { return envelope_; }

    // Called after a successful set_patch or state load (e.g. to request a
    // restart when a latency-changing parameter moved, or rescan values).
    // onBeforeApply runs after validation and before ParamStore::assign. A
    // plugin can defer RT adoption until its onApplied callback publishes an
    // immutable companion snapshot (for example, the patch envelope seed).
    std::function<void(const patch::FlatBody& next)> onBeforeApply;
    std::function<void(const patch::FlatBody& before, const patch::FlatBody& after)> onApplied;

    // Builds a clap_plugin_supple_semantic_params_t whose thunks find the
    // FlatSemantic through `resolve(plugin)`. Keep the returned object alive
    // as long as the plugin (typically a static).
    using Resolver = FlatSemantic* (*)(const clap_plugin_t*);
    template <Resolver R, bool HasTelemetry = false>
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
            HasTelemetry ? +[](const clap_plugin_t* p,
                               const clap_ostream_t* o) { return R(p)->getTelemetry(o); }
                         : nullptr,
            nullptr};
        return &ext;
    }

  private:
    const patch::FlatPatchType& type_;
    ParamStore& store_;
    std::string pluginId_, docs_, describePrefix_;
    std::uint32_t stateMagic_;
    patch::Envelope envelope_;
};

// Serializes errors as the extension's JSON error array.
std::string errorsToJson(const Error& error);

}  // namespace supple::clapx
