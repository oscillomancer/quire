// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A plugin the player runs: a compiled DSP script (supple/script_program.hpp)
// as a framework Definition with a meaning (supple/suite/semantics.h).
//
//   - Identity from the script's Info: its ID, name, vendor, version, kind
//     and buses. The state tag is made from the ID, so it is as permanent as
//     the ID is.
//   - A parameter per Param member, with the script's CLAP ID, ranges, unit,
//     curve, group and flags. A continuous parameter's CLAP value is its
//     0..1 position (normalizedHostValue()), as Quire's semantic hosts keep
//     it; the plain value is what the script reads (a percent as a
//     fraction), so the framework hands it over as it is.
//   - The meaning: each parameter's key (the member's name in snake case,
//     or withKey()), its roles as "role:<name>" tags, choices by key.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "supple/script_program.hpp"
#include "supple/suite/semantics.h"

namespace supple::player {

class ScriptDefinition final : public suite::SemanticDefinition {
  public:
    ScriptDefinition(PluginInfo info, ParamTable params, suite::Meaning meaning,
                     std::shared_ptr<const scriptc::Program> program, std::filesystem::path file,
                     std::string source)
        : SemanticDefinition(std::move(info), std::move(params), {}, std::move(meaning)),
          program_(std::move(program)),
          file_(std::move(file)),
          source_(std::move(source)) {}

    const std::shared_ptr<const scriptc::Program>& program() const { return program_; }
    // Where it came from (empty for an embedded script), and its text.
    const std::filesystem::path& file() const { return file_; }
    const std::string& source() const { return source_; }

  private:
    std::shared_ptr<const scriptc::Program> program_;
    std::filesystem::path file_;
    std::string source_;
};

// The Definition of a compiled script; null when it would break the
// framework's rules (Definition::problems() says why, in `problems`).
std::shared_ptr<const ScriptDefinition> makeScriptDefinition(
    std::shared_ptr<const scriptc::Program> program, std::filesystem::path file, std::string source,
    std::string* problems = nullptr);

// Whether two plugins have the same parameters, kinds, ranges and buses: a
// script whose code changed but whose table did not swaps in place, without
// the host restarting it.
bool sameShape(const Definition& a, const Definition& b);

// Four ASCII characters made from an ID, for the state tag.
std::string stateTagFor(std::string_view id);

}  // namespace supple::player
