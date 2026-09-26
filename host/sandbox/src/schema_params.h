// Parameter specs rebuilt from a semantic schema's "params" entries
// (core-params describeParamJson), for hosts that talk to a plugin only
// through its schema: CLAP value ↔ physical value, normalized mapping.
#pragma once

#include <string>
#include <vector>

#include "supple/json/json.h"
#include "supple/params/params.h"

namespace supple::sandbox {

struct SchemaParam {
    params::ParamSpec spec;               // string members point into `entry` / `choices`
    std::vector<params::Choice> choices;  // enum choices (keys borrowed from the entry)
    std::string unitName;
};

inline params::Curve curveFromName(std::string_view name) {
    params::Curve c;
    if (name == "log") {
        c.kind = params::CurveKind::Log;
    } else if (name.rfind("pow:", 0) == 0) {
        c.kind = params::CurveKind::Pow;
        c.k = std::stod(std::string(name.substr(4)));
    } else if (name.rfind("bipolar:", 0) == 0) {
        c.kind = params::CurveKind::Bipolar;
        c.k = std::stod(std::string(name.substr(8)));
    }
    return c;
}

// `entry` must outlive the result (choice keys are borrowed).
inline SchemaParam schemaParam(const json::Value& entry) {
    SchemaParam p;
    auto str = [&](const char* k) -> std::string_view {
        const json::Value* v = entry.find(k);
        return v && v->isString() ? std::string_view(v->asString()) : std::string_view();
    };
    const std::string_view type = str("type");
    p.spec.type = type == "int"    ? params::Type::Int
                  : type == "bool" ? params::Type::Bool
                  : type == "enum" ? params::Type::Enum
                                   : params::Type::Float;
    if (const json::Value* c = entry.find("choices"); c && c->isArray()) {
        for (const auto& v : c->asArray())
            if (v.isString()) p.choices.push_back({v.asString(), v.asString()});
        p.spec.choices = p.choices;
        p.spec.min = 0.0;
        p.spec.max = static_cast<double>(p.choices.size()) - 1.0;
    }
    if (const json::Value* v = entry.find("min"); v && v->isNumber()) p.spec.min = v->asDouble();
    if (const json::Value* v = entry.find("max"); v && v->isNumber()) p.spec.max = v->asDouble();
    if (p.spec.type == params::Type::Bool) p.spec.max = 1.0;
    p.unitName = std::string(str("unit"));
    for (int u = 0; u <= static_cast<int>(units::Unit::none); ++u)
        if (!p.unitName.empty() && units::unitName(static_cast<units::Unit>(u)) == p.unitName)
            p.spec.unit = static_cast<units::Unit>(u);
    if (!str("curve").empty()) p.spec.curve = curveFromName(str("curve"));
    return p;
}

// The physical value for a CLAP value (ADR-0010: normalized for floats).
inline double physicalFromClap(const SchemaParam& p, double clap) {
    return params::fromClap(p.spec, clap);
}

}  // namespace supple::sandbox
