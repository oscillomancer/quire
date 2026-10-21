// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Plugin-side support for a template-derived plugin with a core-params table
// (ADR-0010, ADR-0011): its values and its CLAP params. The semantic
// extension and the CLAP state are semantic_model.h's.
//
//   ParamStore     physical parameter values shared by the main and audio
//                  threads (wait-free atomics + a change generation).
//   ClapParams     clap_plugin_params_t helpers (info, text) over a table.
//
// Threading: ParamStore reads/writes and generation() are RT-safe; everything
// else is main-thread (CLAP's rules for params info/text). Whole patches
// (set_patch, state load) reach the audio thread atomically through
// readAll(): see ParamStore.
#pragma once

#include <clap/clap.h>

#include <atomic>
#include <memory>
#include <span>
#include <string>

#include "supple/base/rt.h"
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

// Serializes errors as the extension's JSON error array.
std::string errorsToJson(const Error& error);

}  // namespace supple::clapx
