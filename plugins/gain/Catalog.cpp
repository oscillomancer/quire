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
#include "script/gain.cpp"
}  // namespace supple::script::user

namespace {

using Script = supple::script::user::Gain;
using Code = supple::script::detail::NativeCode<Script>;

class OneScript final : public supple::Catalog {
  public:
    std::vector<supple::DefinitionPtr> list() override {
        if (!made_) {
            made_ = true;
            auto code = std::make_shared<const Code>(std::vector<Code::Key>{
                {&Script::gain, "gain"},
            }, std::vector<Code::FileKey>{

            });
            std::string problems;
            definition_ = supple::suite::makeScriptDefinition(std::move(code), {}, {}, &problems);
            if (!definition_) std::fprintf(stderr, "Gain: %s\n", problems.c_str());
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
