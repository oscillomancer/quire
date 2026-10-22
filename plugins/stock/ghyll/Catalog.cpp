// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The plugin this product offers (core/Catalog.h): the script in
// product/script/, compiled as C++ (supple/script_native.h) and run by the
// suite's glue as the script player runs it.
//
// Below, each Param member's patch key. C++ cannot see a member's name,
// which a key is made from, so they are listed by member; a Param missing
// from the list takes the key withKey() gives, or else its name's in
// snake_case. List a Param you add, under the key its member's name gives
// (lowCut: low_cut), and keep a key once sessions use it.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/Catalog.h"
#include "supple/script_native.h"
#include "supple/suite/script_definition.h"

namespace supple::script::user {
#include "script/ghyll.cpp"
}  // namespace supple::script::user

namespace {

using Script = supple::script::user::Ghyll;
using Code = supple::script::detail::NativeCode<Script>;

class OneScript final : public supple::Catalog {
  public:
    std::vector<supple::DefinitionPtr> list() override {
        if (!made_) {
            made_ = true;
            auto code = std::make_shared<const Code>(std::vector<Code::Key>{
                {&Script::roomWidth, "room_width"},
                {&Script::roomDepth, "room_depth"},
                {&Script::roomHeight, "room_height"},
                {&Script::walls, "walls"},
                {&Script::floor, "floor"},
                {&Script::ceiling, "ceiling"},
                {&Script::decay, "decay"},
                {&Script::scatter, "scatter"},
                {&Script::sourceX, "source_x"},
                {&Script::sourceY, "source_y"},
                {&Script::listenerX, "listener_x"},
                {&Script::listenerY, "listener_y"},
                {&Script::mic, "mic"},
                {&Script::predelay, "predelay"},
                {&Script::early, "early"},
                {&Script::late, "late"},
                {&Script::lowCut, "low_cut"},
                {&Script::highCut, "high_cut"},
                {&Script::width, "width"},
                {&Script::freeze, "freeze"},
                {&Script::mix, "mix"},
                {&Script::output, "output"},
                {&Script::inputLevel, "input_level"},
                {&Script::outputLevel, "output_level"},
                {&Script::roomLevel, "room_level"},
                {&Script::rt60, "rt60"},
                {&Script::rt60Low, "rt60_low"},
                {&Script::rt60High, "rt60_high"},
                {&Script::distance, "distance"},
                {&Script::criticalDistance, "critical_distance"},
                {&Script::clarity, "clarity"},
                {&Script::definition, "definition"},
                {&Script::firstReflection, "first_reflection"},
            }, std::vector<Code::FileKey>{

            });
            std::string problems;
            definition_ = supple::suite::makeScriptDefinition(std::move(code), {}, {}, &problems);
            if (!definition_) std::fprintf(stderr, "Ghyll: %s\n", problems.c_str());
        }
        if (!definition_) return {};
        return {definition_};
    }

  private:
    bool made_ = false;
    supple::DefinitionPtr definition_;
};

}  // namespace

std::unique_ptr<supple::Catalog> product::makeCatalog() { return std::make_unique<OneScript>(); }
