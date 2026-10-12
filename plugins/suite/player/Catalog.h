// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Where the player finds scripts, and how it makes plugins of them
// (Catalog.cpp, Layer.cpp).
#pragma once

#include <filesystem>
#include <memory>
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
std::shared_ptr<const ScriptDefinition> compileScript(const std::filesystem::path& file,
                                                      const std::string& source,
                                                      std::string* problems);

}  // namespace supple::player
