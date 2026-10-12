// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The Definition a product with tables makes: product.cmake (ProductInfo.h),
// product/Params.h, product/Properties.h and product/Presets.h, which their
// headers check when this compiles. Not built for a run-time product.
#include <iterator>

#include "core/Definition.h"
#include "core/ParamTable.h"
#include "core/Preset.h"
#include "core/PropertyTable.h"
#include "core/StateFormat.h"
#include "ProductInfo.h"

namespace supple {
namespace {

constexpr size_t savedParams() {
    size_t n = 0;
    for (size_t i = 0; i < params::kCount; ++i) n += params::at(i).isOutput() ? 0 : 1;
    return n;
}
constexpr uint64_t propertyLimits() {
    uint64_t n = 0;
    for (size_t i = 0; i < properties::kCount; ++i) n += properties::at(i).limit;
    return n;
}
static_assert(state::maxSavedBytes(savedParams(), properties::kCount, propertyLimits()) <= state::kMaxBytes,
              "the properties' limits add up to more state than will load back (64 MiB): lower some maxBytes()");

PluginInfo productInfo() {
    namespace info = product::info;
    PluginInfo p;
    p.id = info::kId;
    p.name = info::kName;
    p.vendor = info::kVendor;
    p.url = info::kUrl;
    p.version = info::kVersion;
    p.description = info::kDescription;
    for (const char* const* f = info::kFeatures; *f; ++f) p.features.emplace_back(*f);
    p.stateTag = info::kStateTag;
    p.kind = info::kInstrument ? PluginKind::Instrument : info::kNoteEffect ? PluginKind::NoteEffect : PluginKind::Effect;
    p.layouts.clear();
    if (!info::kNoteEffect)
        for (const auto& l : info::kLayouts) p.layouts.push_back({l.channels, l.layout});
    p.sidechain = info::kSidechain;
    for (int k = 0; k < info::kAuxOutputs; ++k) p.auxOutputs.emplace_back(info::kAuxOutputNames[k]);
    p.noteInput = info::kNoteInput;
    p.noteOutput = info::kNoteOutput;
    p.noteExpressions = info::kNoteExpressions;
    p.presets = info::kPresets;
    p.tuning = info::kTuning;
    return p;
}

std::vector<FactoryPreset> productPresets() {
    std::vector<FactoryPreset> out;
    for (size_t i = 0; i < presets::kFactoryCount; ++i) {
        const Preset& preset = presets::factory(i);
        FactoryPreset f;
        f.name = preset.name;
        f.category = preset.category ? preset.category : "";
        for (const PresetValue& v : preset.values) f.values.emplace_back(v.id, v.plain);
        for (const PresetText& t : preset.texts) f.texts.emplace_back(t.id, t.value);
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace

const DefinitionPtr& productDefinition() {
    static const DefinitionPtr definition = std::make_shared<const Definition>(
        productInfo(), ParamTable(product::kParams, params::kCount),
#if SUPPLE_HAS_PROPERTIES
        PropertyList(product::kProperties, properties::kCount),
#else
        PropertyList(),
#endif
        productPresets());
    return definition;
}

}  // namespace supple
