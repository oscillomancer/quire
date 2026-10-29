// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/suite/script_definition.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace supple::suite {

using script::detail::ParamModel;

std::string stateTagFor(std::string_view id) { return script::detail::stateTag(id); }

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
        case ParamModel::Unit::Meters:
            return Unit::Meters;
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
    if (m.polyModulatable) p = p.polyModulatable();
    if (!m.automatable) p = p.notAutomatable();
    if (m.restarts) p = p.restartOnChange();
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
    info.features.insert(info.features.end(), i.features.begin(), i.features.end());
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
    info.presets = !code->factoryPatches().empty();
    if (const auto found =
            script::detail::factoryPatchProblems(code->params(), code->factoryPatches());
        !found.empty()) {
        if (problems) *problems = found;
        return nullptr;
    }
    // Every CLAP note expression reaches an instrument's voices.
    if (info.instrument()) info.noteExpressions = 0x7F;

    Meaning meaning;
    std::string slug = snakeCase(i.name.empty() ? i.id : i.name);
    for (char& c : slug)
        if (c == '_') c = '-';
    meaning.patchType = "supple." + slug + "-patch/1.0";
    meaning.docs = i.description;
    for (const ParamModel& m : code->params()) {
        ParamMeaning pm;
        pm.key = m.key;
        for (const std::string& role : m.roles) pm.tags.push_back("role:" + role);
        for (size_t k = 0; k < m.choices.size(); ++k)
            pm.choiceKeys.push_back(script::detail::choiceKey(m, k));
        for (const ParamModel::Condition& c : m.activeWhen) {
            ActiveCondition a;
            a.param = c.param < 0 ? meaning.params.size() + 1  // refused below
                                  : static_cast<size_t>(c.param);
            a.values = c.values;
            // A number's range in plain units, as the script reads it (a
            // percent as a fraction).
            const double scale =
                c.param >= 0 && code->params()[static_cast<size_t>(c.param)].unit ==
                                    ParamModel::Unit::Percent
                    ? 0.01
                    : 1.0;
            a.above = c.above * scale;
            a.below = c.below * scale;
            pm.activeWhen.push_back(std::move(a));
        }
        // Routes move a continuous parameter the host may modulate: per
        // voice when the host may modulate it per note.
        if (m.kind == ParamModel::Kind::Continuous && !m.output && m.modulatable)
            pm.routable =
                m.polyModulatable && info.instrument() ? Routable::Voice : Routable::Global;
        pm.routeMin = m.routeMin;
        pm.routeMax = m.routeMax;
        meaning.params.push_back(std::move(pm));
    }
    // The sources: the script's, then an instrument's own.
    for (const script::detail::SourceModel& s : code->sources())
        meaning.sources.push_back({s.key, s.name, s.description, s.perVoice, s.bipolar});
    if (info.instrument())
        for (const script::detail::BuiltinSource& s : script::detail::kInstrumentSources)
            meaning.sources.push_back({s.key, s.name, s.description, s.perVoice, s.bipolar});
    for (const script::detail::FileModel& m : code->files()) meaning.properties.push_back({m.key});
    // With sources and something for them to move, the routes: a property
    // of their own, after the files.
    const bool routes =
        !meaning.sources.empty() &&
        std::any_of(meaning.params.begin(), meaning.params.end(),
                    [](const ParamMeaning& p) { return p.routable != Routable::No; });
    if (routes) {
        meaning.routes = static_cast<int>(meaning.properties.size());
        meaning.properties.push_back({"routes"});
    } else {
        meaning.sources.clear();
    }
    // The tables, made afresh for each definition that holds them.
    auto params = [&] {
        ParamTable::Builder b;
        for (const ParamModel& m : code->params()) b.add(paramOf(m, b));
        return b.build();
    };
    auto properties = [&] {
        // Each file member is a file property: its path, saved and set by patch.
        PropertyList::Builder files;
        for (const script::detail::FileModel& m : code->files()) {
            PropertyDef p = property(m.id, files.text(m.name), files.text(m.def))
                                .audioFile()
                                .ofKind(static_cast<uint32_t>(m.kind));
            if (!m.description.empty()) p = p.help(files.text(m.description));
            files.add(p);
        }
        if (routes)
            files.add(property(kRoutesProperty, "Routes")
                          .maxBytes(16384)
                          .hidden()
                          .help("The modulation routes, one a line: source>parameter amount, "
                                "and the source it goes through if any. A patch sets them as "
                                "its routes member."));
        return files.build();
    };
    // Factory patches' routes, as a patch's would be checked and written.
    std::vector<std::string> factoryRoutes(code->factoryPatches().size());
    if (routes) {
        const SemanticDefinition plain(info, params(), properties(), meaning);
        const PatchCodec codec(plain);
        for (size_t k = 0; k < factoryRoutes.size(); ++k) {
            const auto& patch = code->factoryPatches()[k];
            json::Value given = json::Value::object();
            for (const auto& r : patch.routes) {
                json::Value route = json::Value::object();
                route["amount"] = r.amount;
                if (!r.via.empty()) route["via"] = r.via;
                given[r.route] = std::move(route);
            }
            std::vector<json::Error> errors;
            if (!codec.routeText(given, "/routes", factoryRoutes[k], errors)) {
                if (problems)
                    *problems = "factory patch '" + patch.name + "': " + errors.front().message +
                                " (" + errors.front().hint + ")";
                return nullptr;
            }
        }
    } else {
        for (const auto& patch : code->factoryPatches())
            if (!patch.routes.empty()) {
                if (problems)
                    *problems = "factory patch '" + patch.name +
                                "' has routes, and the plugin has no sources or nothing they move";
                return nullptr;
            }
    }
    std::vector<FactoryPreset> presets;
    for (const auto& patch : code->factoryPatches()) {
        FactoryPreset preset;
        preset.name = patch.name;
        for (const auto& [id, physical] : patch.values) {
            const auto p = std::find_if(code->params().begin(), code->params().end(),
                                        [id](const ParamModel& m) { return m.id == id; });
            preset.values.emplace_back(
                id, physical * (p->unit == ParamModel::Unit::Percent ? 0.01 : 1.0));
        }
        for (const auto& [id, reference] : patch.files) preset.texts.emplace_back(id, reference);
        if (routes) preset.texts.emplace_back(kRoutesProperty, factoryRoutes[presets.size()]);
        presets.push_back(std::move(preset));
    }
    auto definition = std::make_shared<const ScriptDefinition>(
        std::move(info), params(), properties(), std::move(meaning), std::move(code),
        std::move(file), std::move(source), std::move(presets));
    std::string found = definition->problems();
    if (found.empty()) found = meaningProblems(*definition, definition->meaning());
    if (!found.empty()) {
        if (problems) *problems = found;
        return nullptr;
    }
    return definition;
}

namespace {
std::string_view text(const char* s) { return s ? std::string_view(s) : std::string_view(); }
}  // namespace

bool sameShape(const Definition& a, const Definition& b) {
    // The framework's preset table is immutable; patch edits need redefinition.
    if (a.info().presets != b.info().presets || a.presets().size() != b.presets().size())
        return false;
    for (size_t i = 0; i < a.presets().size(); ++i)
        if (a.presets()[i].name != b.presets()[i].name ||
            a.presets()[i].values != b.presets()[i].values ||
            a.presets()[i].texts != b.presets()[i].texts)
            return false;
    // A swap keeps the running Definition, so whatever it says of the plugin
    // -- names, help, keys -- must be what the new code would say too.
    const PluginInfo &x = a.info(), &y = b.info();
    if (x.id != y.id || x.name != y.name || x.vendor != y.vendor || x.url != y.url ||
        x.version != y.version || x.description != y.description || x.features != y.features ||
        x.stateTag != y.stateTag || x.kind != y.kind || x.layouts != y.layouts ||
        x.sidechain != y.sidechain || x.auxOutputs != y.auxOutputs || x.noteInput != y.noteInput ||
        x.noteOutput != y.noteOutput || x.noteExpressions != y.noteExpressions ||
        x.presets != y.presets || x.tuning != y.tuning || x.voices != y.voices ||
        x.overlappingNotes != y.overlappingNotes || a.params().count() != b.params().count() ||
        a.properties().count() != b.properties().count())
        return false;
    for (size_t k = 0; k < a.properties().count(); ++k) {
        const PropertyDef &p = a.properties().at(k), &q = b.properties().at(k);
        if (p.id != q.id || p.kind != q.kind || text(p.name) != text(q.name) ||
            text(p.def) != text(q.def) || p.limit != q.limit || p.flags != q.flags ||
            text(p.extensions) != text(q.extensions) || text(p.description) != text(q.description))
            return false;
    }
    for (size_t k = 0; k < a.params().count(); ++k) {
        const ParamDef &p = a.params().at(k), &q = b.params().at(k);
        if (p.id != q.id || p.kind != q.kind || p.unit != q.unit || p.taper != q.taper ||
            p.min != q.min || p.max != q.max || p.def != q.def || p.curve != q.curve ||
            p.precision != q.precision || p.flags != q.flags || p.smoothing != q.smoothing ||
            text(p.name) != text(q.name) || text(p.group) != text(q.group) ||
            text(p.label) != text(q.label) || text(p.suffix) != text(q.suffix) ||
            text(p.description) != text(q.description) || p.formatter != q.formatter ||
            p.parser != q.parser || p.labelCount != q.labelCount)
            return false;
        for (int c = 0; c < p.labelCount; ++c)
            if (text(p.labels[c]) != text(q.labels[c])) return false;
    }
    const auto* m = dynamic_cast<const SemanticDefinition*>(&a);
    const auto* n = dynamic_cast<const SemanticDefinition*>(&b);
    if ((m == nullptr) != (n == nullptr) || (m && m->meaning() != n->meaning())) return false;
    // The latency and tail the host was told stay until it restarts us.
    const auto* s = dynamic_cast<const ScriptDefinition*>(&a);
    const auto* t = dynamic_cast<const ScriptDefinition*>(&b);
    if (s && t &&
        (s->code()->info().latency != t->code()->info().latency ||
         s->code()->info().tail != t->code()->info().tail))
        return false;
    return true;
}

}  // namespace supple::suite
