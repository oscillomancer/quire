// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The preset library, for a product with PRODUCT_PRESETS ON: factory presets
// (core/Preset.h), the user's own in a per-platform folder, which one is
// loaded and whether it has been changed since, A/B comparison, and the whole
// state as text for the clipboard. Main thread only; the editor's preset bar
// drives it, and CLAP's preset-load extension and discovery factory read it.
//
// A preset is a state blob in the preset context (core/StateFormat.h), and a
// user preset is exactly that in a file named after it, so "modified" is a
// byte comparison with what the preset loaded: state round-trips exactly.
//
// User folders:
//   macOS    ~/Library/Audio/Presets/<vendor>/<product>
//   Windows  %APPDATA%\<vendor>\<product>\Presets
//   Linux    $XDG_DATA_HOME (or ~/.local/share)/<vendor>/<product>/presets
// and SUPPLE_PRESETS_DIR in the environment replaces them all (for tests).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/StateFormat.h"

namespace supple {

// What the library needs from its owner: the state, saved and loaded.
class StateAccess {
  public:
    virtual ~StateAccess() = default;
    virtual std::vector<uint8_t> saveState(state::Context context) = 0;
    // Applies a whole blob, or nothing (false).
    virtual bool loadState(const uint8_t* data, size_t size, state::Context context) = 0;
    // The chunks saveState() puts after the framework's state (a layer's),
    // for a snapshot the library makes itself -- the defaults, a factory
    // preset -- so that it compares with saveState() byte for byte.
    virtual void appendChunks(std::vector<uint8_t>& bytes, const state::Snapshot& snapshot,
                              state::Context context) {
        (void)bytes, (void)snapshot, (void)context;
    }
};

class PresetLibrary {
  public:
    static constexpr const char* kExtension = ".preset";
    static constexpr size_t kMaxName = 64;

    struct Entry {
        std::string name;
        std::string category;   // factory presets only; "" for none
        int factory = -1;       // index in the factory table, or -1 for a user preset
        std::filesystem::path file;
        bool user() const { return factory < 0; }
    };

    // An empty folder means no user presets (the platform folder could not
    // be found). The factory presets, the state tag and the names are the
    // definition's.
    PresetLibrary(StateAccess& state, std::filesystem::path userFolder,
                  DefinitionPtr definition = productDefinition());

    // The platform's folder for this plugin's presets.
    static std::filesystem::path defaultUserFolder(const PluginInfo& info);
#if !SUPPLE_PRODUCT_RUNTIME
    // A product with tables: its own.
    static std::filesystem::path defaultUserFolder();
#endif
    static std::filesystem::path fromUtf8(std::string_view text);
    static std::string toUtf8(const std::filesystem::path& path);
    // A name that can be a file on every platform: 1 to 64 bytes, no
    // control characters, none of / \ : * ? " < > |, no leading or trailing
    // space or dot, not a reserved Windows device name.
    static bool validName(std::string_view name);

    // --- the list: factory presets in table order, then the user's by name ---
    void refresh();
    const std::vector<Entry>& entries() const { return entries_; }
    // Changes whenever entries() may have, for what is built from the list.
    uint64_t revision() const { return revision_; }
    const std::filesystem::path& userFolder() const { return folder_; }

    // --- what is loaded -------------------------------------------------------
    // Index in entries(), or -1 (the defaults, pasted text, a deleted preset).
    int current() const { return current_ >= 0 && static_cast<size_t>(current_) < entries_.size() ? current_ : -1; }
    // "Default" for the defaults, "" when nothing named is loaded.
    const std::string& currentName() const { return name_; }
    // The state differs from what the current preset (or the defaults) set.
    bool modified();

    bool load(int index);
    bool step(int delta);  // the next or previous entry, wrapping
    bool loadFactory(size_t index);
    bool loadFile(const std::filesystem::path& file);
    // The state, as a preset file anywhere (an export); the library's own
    // folder is not involved.
    bool exportFile(const std::filesystem::path& file);
    bool loadDefaults();

    // --- the user's presets ----------------------------------------------------
    enum class SaveResult { Ok, InvalidName, Exists, Failed };
    // Saves the current state as a user preset, which becomes current.
    SaveResult save(std::string_view name, bool overwrite);
    bool rename(int index, std::string_view name);
    bool remove(int index);
    bool canSave() const { return !folder_.empty(); }

    // --- A/B ----------------------------------------------------------------------
    // Two whole states to switch between; B starts as a copy of A.
    int slot() const { return slot_; }
    void compare(int slot);
    void copyToOther();

    // --- text -------------------------------------------------------------------------
    // The state (preset context) as one line of text, and back.
    std::string copyText();
    bool pasteText(std::string_view text);

    // --- the session --------------------------------------------------------------------
    // From and into a saved session (state::Snapshot's preset fields).
    void restore(const state::Snapshot& session);
    void describe(state::Snapshot& session);

    // Called after the library loaded something itself (not for a host's
    // preset-load request), for CLAP's host preset-load.loaded().
    std::function<void(const Entry&)> onLoaded;

  private:
    struct Slot {
        bool used = false;
        std::vector<uint8_t> state;
        std::string name;
        std::string factoryName;
        std::filesystem::path file;
        std::vector<uint8_t> baseline;
        bool stickyModified = false;
    };

    bool apply(const std::vector<uint8_t>& blob);
    void loaded(int index, std::string name);
    int findEntry(std::string_view name, bool user) const;
    Slot capture();
    std::filesystem::path fileFor(std::string_view name) const;
    std::vector<uint8_t> factoryBlob(size_t index) const;
    std::vector<uint8_t> defaultsBlob() const;
    std::vector<uint8_t> blob(const state::Snapshot& snapshot) const;

    StateAccess& state_;
    DefinitionPtr definition_;
    uint32_t magic_;
    std::filesystem::path folder_;
    std::vector<Entry> entries_;
    uint64_t revision_ = 0;
    int current_ = -1;
    std::string name_ = "Default";
    std::vector<uint8_t> baseline_;  // the preset state as loaded; empty: not known
    bool stickyModified_ = false;    // restored as modified from a session
    Slot slots_[2];
    int slot_ = 0;
};

namespace base64 {
std::string encode(const std::vector<uint8_t>& bytes);
bool decode(std::string_view text, std::vector<uint8_t>& out);
}  // namespace base64

}  // namespace supple
