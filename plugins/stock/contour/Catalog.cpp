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
#include "script/contour.cpp"
}  // namespace supple::script::user

namespace {

using Script = supple::script::user::Contour;
using Code = supple::script::detail::NativeCode<Script>;

class OneScript final : public supple::Catalog {
  public:
    std::vector<supple::DefinitionPtr> list() override {
        if (!made_) {
            made_ = true;
            auto code = std::make_shared<const Code>(std::vector<Code::Key>{
                {&Script::b1On, "band1.enabled"},
                {&Script::b1Type, "band1.type"},
                {&Script::b1Freq, "band1.freq"},
                {&Script::b1Gain, "band1.gain"},
                {&Script::b1Q, "band1.q"},
                {&Script::b1Slope, "band1.slope"},
                {&Script::b1Channel, "band1.channel"},
                {&Script::b1Dynamic, "band1.dynamic"},
                {&Script::b1Threshold, "band1.threshold"},
                {&Script::b2On, "band2.enabled"},
                {&Script::b2Type, "band2.type"},
                {&Script::b2Freq, "band2.freq"},
                {&Script::b2Gain, "band2.gain"},
                {&Script::b2Q, "band2.q"},
                {&Script::b2Slope, "band2.slope"},
                {&Script::b2Channel, "band2.channel"},
                {&Script::b2Dynamic, "band2.dynamic"},
                {&Script::b2Threshold, "band2.threshold"},
                {&Script::b3On, "band3.enabled"},
                {&Script::b3Type, "band3.type"},
                {&Script::b3Freq, "band3.freq"},
                {&Script::b3Gain, "band3.gain"},
                {&Script::b3Q, "band3.q"},
                {&Script::b3Slope, "band3.slope"},
                {&Script::b3Channel, "band3.channel"},
                {&Script::b3Dynamic, "band3.dynamic"},
                {&Script::b3Threshold, "band3.threshold"},
                {&Script::b4On, "band4.enabled"},
                {&Script::b4Type, "band4.type"},
                {&Script::b4Freq, "band4.freq"},
                {&Script::b4Gain, "band4.gain"},
                {&Script::b4Q, "band4.q"},
                {&Script::b4Slope, "band4.slope"},
                {&Script::b4Channel, "band4.channel"},
                {&Script::b4Dynamic, "band4.dynamic"},
                {&Script::b4Threshold, "band4.threshold"},
                {&Script::b5On, "band5.enabled"},
                {&Script::b5Type, "band5.type"},
                {&Script::b5Freq, "band5.freq"},
                {&Script::b5Gain, "band5.gain"},
                {&Script::b5Q, "band5.q"},
                {&Script::b5Slope, "band5.slope"},
                {&Script::b5Channel, "band5.channel"},
                {&Script::b5Dynamic, "band5.dynamic"},
                {&Script::b5Threshold, "band5.threshold"},
                {&Script::b6On, "band6.enabled"},
                {&Script::b6Type, "band6.type"},
                {&Script::b6Freq, "band6.freq"},
                {&Script::b6Gain, "band6.gain"},
                {&Script::b6Q, "band6.q"},
                {&Script::b6Slope, "band6.slope"},
                {&Script::b6Channel, "band6.channel"},
                {&Script::b6Dynamic, "band6.dynamic"},
                {&Script::b6Threshold, "band6.threshold"},
                {&Script::b7On, "band7.enabled"},
                {&Script::b7Type, "band7.type"},
                {&Script::b7Freq, "band7.freq"},
                {&Script::b7Gain, "band7.gain"},
                {&Script::b7Q, "band7.q"},
                {&Script::b7Slope, "band7.slope"},
                {&Script::b7Channel, "band7.channel"},
                {&Script::b7Dynamic, "band7.dynamic"},
                {&Script::b7Threshold, "band7.threshold"},
                {&Script::b8On, "band8.enabled"},
                {&Script::b8Type, "band8.type"},
                {&Script::b8Freq, "band8.freq"},
                {&Script::b8Gain, "band8.gain"},
                {&Script::b8Q, "band8.q"},
                {&Script::b8Slope, "band8.slope"},
                {&Script::b8Channel, "band8.channel"},
                {&Script::b8Dynamic, "band8.dynamic"},
                {&Script::b8Threshold, "band8.threshold"},
                {&Script::scale, "scale"},
                {&Script::autoGain, "auto_gain"},
                {&Script::output, "output"},
                {&Script::listen, "listen"},
            }, std::vector<Code::FileKey>{

            });
            std::string problems;
            definition_ = supple::suite::makeScriptDefinition(std::move(code), {}, {}, &problems);
            if (!definition_) std::fprintf(stderr, "Contour: %s\n", problems.c_str());
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
