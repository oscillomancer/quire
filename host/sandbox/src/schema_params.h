// Parameter specs rebuilt from a semantic schema's "params" entries
// (core-params describeParamJson), for hosts that talk to a plugin only
// through its schema: CLAP value ↔ physical value, normalized mapping.
#pragma once

#include <charconv>
#include <cmath>
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

inline Result<params::Curve> curveFromName(std::string_view name) {
    params::Curve c;
    if (name == "linear" || name.empty()) return c;
    if (name == "log") {
        c.kind = params::CurveKind::Log;
    } else if (name.rfind("pow:", 0) == 0) {
        c.kind = params::CurveKind::Pow;
        name.remove_prefix(4);
    } else if (name.rfind("bipolar:", 0) == 0) {
        c.kind = params::CurveKind::Bipolar;
        name.remove_prefix(8);
    } else {
        return Error("sandbox.param_curve", "unknown parameter curve", "/curve",
                     "use linear, log, pow:<positive exponent>, or bipolar:<positive exponent>");
    }
    if (c.kind == params::CurveKind::Pow || c.kind == params::CurveKind::Bipolar) {
        const char* begin = name.data();
        const char* end = begin + name.size();
        const auto [next, err] = std::from_chars(begin, end, c.k);
        if (err != std::errc{} || next != end || !std::isfinite(c.k) || c.k <= 0.0)
            return Error("sandbox.param_curve", "curve exponent must be finite and positive",
                         "/curve", "use e.g. pow:3 or bipolar:2");
    }
    return c;
}

// `entry` must outlive the result (choice keys are borrowed).
inline Result<SchemaParam> schemaParam(const json::Value& entry) {
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
    if (!str("curve").empty()) {
        auto curve = curveFromName(str("curve"));
        if (!curve) return std::move(curve).error();
        p.spec.curve = *curve;
    }
    return p;
}

// The physical value for a CLAP value (ADR-0010: normalized for floats).
inline double physicalFromClap(const SchemaParam& p, double clap) {
    return params::fromClap(p.spec, clap);
}

}  // namespace supple::sandbox
