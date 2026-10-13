// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Where the player finds scripts, and how it makes plugins of them
// (Catalog.cpp, Layer.cpp).
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ScriptDefinition.h"

namespace supple::player {

// The longest script read: a page of C++ is a few kilobytes.
inline constexpr size_t kMaxScriptBytes = 1u << 20;

// The folders SUPPLE_SCRIPT_LIBRARY names, in order (':' between them; ';'
// on Windows), or none.
std::vector<std::filesystem::path> libraryFolders();
// <folder>/<name>/<name>.cpp for every <name>, sorted.
std::vector<std::filesystem::path> libraryScripts(const std::filesystem::path& folder);
// A script's whole text; false when it cannot be read or is too long.
bool readScript(const std::filesystem::path& file, std::string& text);
// A script compiled into a plugin (the interpreter instead of machine code
// when SUPPLE_SCRIPT_INTERPRET is set); null with the reasons in `problems`.
// `file` is where it came from, which the layer follows; empty for an
// embedded script.
std::shared_ptr<const ScriptDefinition> compileScript(const std::filesystem::path& file,
                                                      const std::string& source,
                                                      std::string* problems);

// The file of the module this code is in (the player's .clap), or empty.
std::filesystem::path ownModule();
// The script an export carries at the end of its module file
// (supple/script_export.hpp), or nothing: a player, not an export.
struct EmbeddedScript {
    std::string name, source;
};
std::optional<EmbeddedScript> embeddedScript(const std::filesystem::path& module);

}  // namespace supple::player
