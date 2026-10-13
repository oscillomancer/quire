// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/suite/script_definition.h"

#include <cmath>

namespace supple::suite {

using script::detail::ParamModel;

std::string stateTagFor(std::string_view id) {
    uint32_t h = 2166136261u;
    for (char c : id) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    static constexpr char kLetters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string tag;
    for (int k = 0; k < 4; ++k) {
        tag += kLetters[h % 52];
        h = h / 52 + static_cast<uint32_t>(k) * 7919u;
    }
    return tag;
}

namespace {

Unit unitOf(ParamModel::Unit u) {
    switch (u) {
        case ParamModel::Unit::Decibels:
            return Unit::Decibels;
        case ParamModel::Unit::Hertz:
            return Unit::Hertz;
        case ParamModel::Unit::Milliseconds:
            return Unit::Milliseconds;
        case ParamModel::Unit::Percent:
            return Unit::Percent;
        case ParamModel::Unit::Ratio:
            return Unit::Ratio;
        case ParamModel::Unit::Semitones:
            return Unit::Semitones;
        case ParamModel::Unit::Cents:
            return Unit::Cents;
        case ParamModel::Unit::Octaves:
            return Unit::Octaves;
        case ParamModel::Unit::Degrees:
            return Unit::Degrees;
        case ParamModel::Unit::Bpm:
            return Unit::Bpm;
        case ParamModel::Unit::Samples:
            return Unit::Samples;
        default:
            return Unit::None;
    }
}

ParamDef paramOf(const ParamModel& m, ParamTable::Builder& b) {
    // The model is physical (a percent in percent); the framework's plain
    // value is what the script reads (a percent as a fraction).
    const double scale = m.unit == ParamModel::Unit::Percent ? 0.01 : 1.0;
    ParamDef p = continuous(m.id, b.text(m.name), m.min * scale, m.max * scale, m.def * scale,
                            unitOf(m.unit));
    switch (m.kind) {
        case ParamModel::Kind::Integer:
            p.kind = ParamKind::Integer;
            break;
        case ParamModel::Kind::Toggle:
            p.kind = ParamKind::Toggle;
            p.labels = kOffOnLabels;
            p.labelCount = 2;
            break;
        case ParamModel::Kind::Choice: {
            std::vector<std::string> labels(m.choices.begin(), m.choices.end());
            p.kind = ParamKind::Choice;
            p.labels = b.labels(labels);
            p.labelCount = static_cast<int>(labels.size());
            break;
        }
        default:
            p.taper = m.curve == ParamModel::Curve::Log   ? Taper::Log
                      : m.curve == ParamModel::Curve::Pow ? Taper::Power
                                                          : Taper::Linear;
            p.curve = m.curve == ParamModel::Curve::Pow ? m.curveK : 1.0;
            p = p.normalizedHostValue();
            if (m.smoothingMs > 0.0 && !m.output) p = p.smoothed(m.smoothingMs);
    }
    if (!m.group.empty()) p = p.in(b.text(m.group));
    if (!m.description.empty()) p = p.help(b.text(m.description));
    if (m.modulatable) p = p.modulatable();
    if (!m.automatable) p = p.notAutomatable();
    if (m.offAtMinimum) p = p.offAtMinimum();
    if (m.bypass) p = p.bypass();
    if (m.hidden) p.flags |= pflag::kHidden;
    if (m.output) p = p.output();
    return p;
}

}  // namespace

std::shared_ptr<const ScriptDefinition> makeScriptDefinition(
    std::shared_ptr<const script::detail::Code> code, std::filesystem::path file,
    std::string source, std::string* problems) {
    using script::detail::PluginKind;
    const script::detail::InfoModel& i = code->info();
    PluginInfo info;
    info.id = i.id;
    info.name = i.name;
    info.vendor = i.vendor;
    info.url = i.url;
    info.version = i.version;
    info.description = i.description;
    info.stateTag = stateTagFor(i.id);
    info.kind = code->kind() == PluginKind::Instrument   ? supple::PluginKind::Instrument
                : code->kind() == PluginKind::NoteEffect ? supple::PluginKind::NoteEffect
                                                         : supple::PluginKind::Effect;
    info.features = {info.instrument()   ? "instrument"
                     : info.noteEffect() ? "note-effect"
                                         : "audio-effect"};
    info.layouts.clear();
    if (!info.noteEffect()) {
        std::vector<int> widths = i.channels;
        if (widths.empty())
            widths = info.instrument() ? std::vector<int>{2} : std::vector<int>{2, 1};
        for (int w : widths)
            info.layouts.push_back({w, w == 1   ? Layout::Mono
                                       : w == 2 ? Layout::Stereo
                                                : Layout::Generic});
    }
    info.sidechain = i.sidechain;
    info.auxOutputs = i.outputs;
    info.voices = i.voices;

    ParamTable::Builder b;
    Meaning meaning;
    std::string slug = snakeCase(i.name.empty() ? i.id : i.name);
    for (char& c : slug)
        if (c == '_') c = '-';
    meaning.patchType = "supple." + slug + "-patch/1.0";
    meaning.docs = i.description;
    for (const ParamModel& m : code->params()) {
        b.add(paramOf(m, b));
        ParamMeaning pm;
        pm.key = m.key;
        for (const std::string& role : m.roles) pm.tags.push_back("role:" + role);
        for (const std::string& choice : m.choices) pm.choiceKeys.push_back(snakeCase(choice));
        meaning.params.push_back(std::move(pm));
    }
    auto definition = std::make_shared<const ScriptDefinition>(std::move(info), b.build(),
                                                               std::move(meaning), std::move(code),
                                                               std::move(file), std::move(source));
    std::string found = definition->problems();
    if (found.empty()) found = meaningProblems(*definition, definition->meaning());
    if (!found.empty()) {
        if (problems) *problems = found;
        return nullptr;
    }
    return definition;
}

bool sameShape(const Definition& a, const Definition& b) {
    const PluginInfo &x = a.info(), &y = b.info();
    if (x.id != y.id || x.name != y.name || x.kind != y.kind || x.layouts != y.layouts ||
        x.sidechain != y.sidechain || x.auxOutputs != y.auxOutputs || x.voices != y.voices ||
        a.params().count() != b.params().count())
        return false;
    for (size_t k = 0; k < a.params().count(); ++k) {
        const ParamDef &p = a.params().at(k), &q = b.params().at(k);
        if (p.id != q.id || p.kind != q.kind || p.unit != q.unit || p.taper != q.taper ||
            p.min != q.min || p.max != q.max || p.def != q.def || p.curve != q.curve ||
            p.flags != q.flags || p.smoothing != q.smoothing || std::string(p.name) != q.name ||
            std::string(p.group) != q.group || p.labelCount != q.labelCount)
            return false;
        for (int c = 0; c < p.labelCount; ++c)
            if (std::string(p.labels[c]) != q.labels[c]) return false;
    }
    return true;
}

}  // namespace supple::suite
