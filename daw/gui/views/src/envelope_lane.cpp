#include "supple/views/envelope_lane.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "supple/model/env_shape.h"
#include "supple/ui/controls.h"

namespace supple::views {

double EnvScale::toNorm(double value) const {
    switch (kind) {
        case Kind::Volume:
            return ui::faderPosition(value);
        case Kind::Pan:
            return std::clamp((value + 100.0) / 200.0, 0.0, 1.0);
        case Kind::Mute:
            return value >= 0.5 ? 1.0 : 0.0;
        case Kind::Plugin:
            return param ? params::toNormalized(param->spec, value) : 0.0;
    }
    return 0.0;
}

double EnvScale::reference() const { return kind == Kind::Plugin && param ? param->spec.def : 0.0; }

double EnvScale::fromNorm(double norm) const {
    const double n = std::clamp(norm, 0.0, 1.0);
    switch (kind) {
        case Kind::Volume:
            return std::max(-150.0, ui::faderDb(n));  // the floor stands for -inf
        case Kind::Pan:
            return std::round(n * 200.0 - 100.0);
        case Kind::Mute:
            return n >= 0.5 ? 1.0 : 0.0;
        case Kind::Plugin:
            return param
                       ? params::clampPhysical(param->spec, params::fromNormalized(param->spec, n))
                       : 0.0;
    }
    return 0.0;
}

std::string EnvScale::text(double value) const {
    char buf[32];
    switch (kind) {
        case Kind::Volume:
            if (value <= -150.0 || !std::isfinite(value)) return "-inf dB";
            std::snprintf(buf, sizeof buf, "%+.1f dB", value);
            return buf;
        case Kind::Pan: {
            const long p = std::lround(value);
            if (p == 0) return "C";
            std::snprintf(buf, sizeof buf, "%s %ld", p < 0 ? "L" : "R", std::labs(p));
            return buf;
        }
        case Kind::Mute:
            return value >= 0.5 ? "muted" : "on";
        case Kind::Plugin:
            return param ? params::format(param->spec, value) : std::string{};
    }
    return {};
}

std::optional<EnvScale> envScale(const model::Envelope& env) {
    const auto kind = env.target.object.kind;
    if (kind != model::Kind::Track && kind != model::Kind::Route && kind != model::Kind::Take)
        return std::nullopt;
    EnvScale s;
    if (env.target.param == "volume")
        s.kind = EnvScale::Kind::Volume;
    else if (env.target.param == "pan")
        s.kind = EnvScale::Kind::Pan;
    else if (env.target.param == "mute")
        s.kind = EnvScale::Kind::Mute;
    else
        return std::nullopt;
    return s;
}

std::string envTitle(const model::Envelope& env) {
    const std::string& p = env.target.param;
    if (p == "volume") return "Volume";
    if (p == "pan") return "Pan";
    if (p == "mute") return "Mute";
    return p;
}

double envValueAt(const model::Envelope& env, double beat, double fallback) {
    const auto& pts = env.points;
    if (pts.empty()) return fallback;
    if (beat <= pts.front().pos) return pts.front().value;
    if (beat >= pts.back().pos) return pts.back().value;
    const auto next = std::upper_bound(
        pts.begin(), pts.end(), beat, [](double b, const model::EnvPoint& p) { return b < p.pos; });
    const model::EnvPoint& b = *next;
    const model::EnvPoint& a = *(next - 1);
    if (b.pos <= a.pos) return b.value;
    const double t = (beat - a.pos) / (b.pos - a.pos);
    return a.value + (b.value - a.value) * model::shapeCurve(a.shape, a.tension, t);
}

int envOrder(const model::Envelope& env) {
    const std::string& p = env.target.param;
    return p == "volume" ? 0 : p == "pan" ? 1 : p == "mute" ? 2 : 3;
}

}  // namespace supple::views
