// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The semantic-params extension (supple/clap/semantic_params.h) for any
// plugin of the framework, as a layer (plugin/Layer.h): the schema, the
// patch read, replaced, merged and validated, and a description, all from
// the instance's Definition and its meaning (supple/suite/semantics.h). And
// the editor-picture extension (supple/clap/editor_picture.h): the editor
// drawn without a window, for a host that cannot look at a screen.
//
// The patch also travels in the state, as a chunk ("PTCH", framed as
// core-patch §4) after the framework's: loading prefers its values by key to
// the framework's by ID, so a session survives a parameter's ID changing,
// and keeps the patch's name, author, tags and notes. A key the plugin no
// longer has is skipped there, not refused: a session must load.
//
// A product's layer derives from this one to add its own work (the player's
// reloading), calling the base for what it overrides. Main thread only.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "plugin/Layer.h"
#include "supple/clap/editor_picture.h"
#include "supple/clap/semantic_params.h"
#include "supple/suite/semantics.h"

namespace supple::suite {

class SemanticLayer : public plugin::Layer {
  public:
    static constexpr uint32_t kPatchChunk = state::fourcc("PTCH");

    explicit SemanticLayer(plugin::Instance& instance);

    const void* extension(const char* id) override;
    uint32_t chunkTag() const override { return kPatchChunk; }
    std::vector<uint8_t> saveChunk(const state::Snapshot& snapshot,
                                   state::Context context) override;
    bool loadChunk(const uint8_t* data, size_t size, state::Snapshot& snapshot,
                   state::Context context) override;
    void redefined() override;

    // The extension's calls.
    bool getSchema(const clap_ostream_t* out) const;
    bool getPatch(uint32_t detail, const clap_ostream_t* out) const;
    bool setPatch(uint32_t mode, const char* json, uint32_t size, const clap_ostream_t* errors);
    bool describe(uint32_t detail, const clap_ostream_t* out) const;
    void watchEditor(bool on);
    bool drawEditor(double scale, const clap_supple_pointer_input_t* input, uint32_t count,
                    const clap_ostream_t* out, const clap_ostream_t* why);

    // What the extension works from; null while the meaning has problems
    // (the extension is then not offered: hosts use plain CLAP).
    const PatchCodec* codec() const { return codec_.get(); }
    const Envelope& envelope() const { return envelope_; }

  protected:
    plugin::Instance& instance() { return instance_; }
    // Whether file property `index` can use the existing file at `path`, and
    // if not why, before a patch sets it: a layer whose files are decoded
    // checks that they decode.
    virtual bool fileReadable(size_t index, const std::string& path, std::string& why) const {
        (void)index, (void)path, (void)why;
        return true;
    }
    // Tells a host that understands the extension what changed.
    void changed(uint32_t flags);

  private:
    void rebuild();

    plugin::Instance& instance_;
    std::unique_ptr<PatchCodec> codec_;
    Envelope envelope_;
};

// Writes all of `text` to a CLAP stream.
bool writeAll(const clap_ostream_t* out, const std::string& text);

}  // namespace supple::suite
