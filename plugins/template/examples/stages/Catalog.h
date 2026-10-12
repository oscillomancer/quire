// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What Stages' plugins are, decided when it runs. Each is a chain of gain
// stages, one parameter each, after an Output level. The built-in plugin
// (the product's own ID) has two, Low and High. A folder of plugin files --
// SUPPLE_STAGES_DIR in the environment -- adds one plugin per `<name>.stages`
// file, whose lines name its stages, in order, up to kMaxStages:
//
//   Body
//   Air
//
// is the plugin "Stages Wide" for wide.stages, ID com.suppleaudio.stages.wide
// (the product's ID, a dot, the file's name in lower case).
//
// A stage's parameter ID is its place in the file (kStage0 + k): editing a
// file renumbers what moves, and the layer keeps values by name across that
// (Layer.cpp).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/Definition.h"

namespace product {

enum ParamId : uint32_t { kOutput = 1, kStage0 = 100 };
inline constexpr int kMaxStages = 16;

// A plugin of Stages: a Definition that knows its stages and its file.
class StagesDefinition final : public supple::Definition {
  public:
    StagesDefinition(supple::PluginInfo info, supple::ParamTable params, std::vector<std::string> stages,
                     std::filesystem::path file)
        : Definition(std::move(info), std::move(params)), stages_(std::move(stages)), file_(std::move(file)) {}

    const std::vector<std::string>& stages() const { return stages_; }
    // The file it was made from; empty for the built-in plugin.
    const std::filesystem::path& file() const { return file_; }

  private:
    std::vector<std::string> stages_;
    std::filesystem::path file_;
};

// A plugin with these stages. `id` and `name` are its identity; `file` is
// where it came from (empty for the built-in one).
supple::DefinitionPtr makeStages(std::string id, std::string name, std::vector<std::string> stages,
                                 std::filesystem::path file = {});

// The stage names a plugin file lists (blank lines and surrounding spaces
// dropped; at most kMaxStages), or none when it cannot be read.
std::vector<std::string> readStages(const std::filesystem::path& file);

}  // namespace product
