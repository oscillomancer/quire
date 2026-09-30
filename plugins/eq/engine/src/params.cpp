#include "eq/params.h"
#include <sstream>
namespace supple::eq {
namespace {
constexpr params::Choice types[] = {{"bell", "Bell"},
                                    {"low_shelf", "Low shelf"},
                                    {"high_shelf", "High shelf"},
                                    {"high_pass", "High-pass"},
                                    {"low_pass", "Low-pass"},
                                    {"notch", "Notch"},
                                    {"band_pass", "Band-pass"},
                                    {"tilt", "Tilt"}};
constexpr params::Choice slopes[] = {
    {"db6", "6 dB/oct"},   {"db12", "12 dB/oct"}, {"db18", "18 dB/oct"}, {"db24", "24 dB/oct"},
    {"db36", "36 dB/oct"}, {"db48", "48 dB/oct"}, {"db72", "72 dB/oct"}, {"db96", "96 dB/oct"}};
constexpr params::Choice channels[] = {
    {"stereo", "Stereo"}, {"left", "Left"}, {"right", "Right"}, {"mid", "Mid"}, {"side", "Side"}};
constexpr params::Choice phases[] = {
    {"minimum", "Minimum"}, {"natural", "Natural"}, {"linear", "Linear"}};
constexpr std::string_view level[] = {"role:level"}, brightness[] = {"role:brightness"},
                           mode[] = {"role:mode"}, width[] = {"role:width"};
struct Definition {
    std::array<std::string, kParamCount> keys, names, groups;
    std::array<params::ParamSpec, kParamCount> specs{};
    Definition() {
        constexpr const char* fields[] = {"enabled", "type",  "freq",   "gain",
                                          "q",       "slope", "channel"};
        constexpr const char* labels[] = {"Enabled", "Type",  "Frequency", "Gain",
                                          "Q",       "Slope", "Channel"};
        constexpr double freqs[] = {30, 200, 1000, 8000, 500, 2000, 4000, 12000};
        for (std::size_t b = 0; b < kBands; ++b)
            for (std::size_t f = 0; f < kBandParams; ++f) {
                const auto i = b * kBandParams + f;
                keys[i] = "band" + std::to_string(b + 1) + "." + fields[f];
                groups[i] = "Band " + std::to_string(b + 1);
                names[i] = groups[i] + " " + labels[f];
                auto& p = specs[i];
                p.id = static_cast<std::uint32_t>(i);
                p.key = keys[i];
                p.name = names[i];
                p.group = groups[i];
                p.description = labels[f];
                p.tags = mode;
                switch (f) {
                    case Enabled:
                        p.type = params::Type::Bool;
                        p.def = b < 4 ? 1 : 0;
                        break;
                    case Type:
                        p.type = params::Type::Enum;
                        p.max = 7;
                        p.def = b == 0 ? HighPass : (b == 3 ? HighShelf : Bell);
                        p.choices = types;
                        break;
                    case Freq:
                        p.unit = units::Unit::Hz;
                        p.min = 10;
                        p.max = 30000;
                        p.def = freqs[b];
                        p.curve.kind = params::CurveKind::Log;
                        p.tags = brightness;
                        p.smoothingMs = 20;
                        break;
                    case Gain:
                        p.unit = units::Unit::dB;
                        p.min = -30;
                        p.max = 30;
                        p.tags = level;
                        p.smoothingMs = 20;
                        break;
                    case Q:
                        p.min = 0.1;
                        p.max = 24;
                        p.def = 0.71;
                        p.curve.kind = params::CurveKind::Log;
                        p.tags = width;
                        p.smoothingMs = 20;
                        break;
                    case Slope:
                        p.type = params::Type::Enum;
                        p.max = 7;
                        p.def = 3;
                        p.choices = slopes;
                        break;
                    case Channel:
                        p.type = params::Type::Enum;
                        p.max = 4;
                        p.choices = channels;
                        break;
                }
            }
        specs[kPhase] = {.id = kPhase,
                         .key = "phase",
                         .name = "Phase",
                         .group = "Output",
                         .type = params::Type::Enum,
                         .max = 2,
                         .def = 1,
                         .choices = phases,
                         .tags = mode,
                         .description = "Phase mode; changes require reactivation.",
                         .flags = 0};
        specs[kAutoGain] = {.id = kAutoGain,
                            .key = "auto_gain",
                            .name = "Auto gain",
                            .group = "Output",
                            .type = params::Type::Bool,
                            .tags = level,
                            .description = "Pink-weighted static curve compensation."};
        specs[kOutput] = {.id = kOutput,
                          .key = "output",
                          .name = "Output",
                          .group = "Output",
                          .unit = units::Unit::dB,
                          .min = -24,
                          .max = 24,
                          .tags = level,
                          .description = "Output gain.",
                          .smoothingMs = 20};
    }
};
const Definition& definition() {
    static const Definition d;
    return d;
}
}  // namespace
params::Table table() { return definition().specs; }
Values defaults() {
    Values v{};
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = table()[i].def;
    return v;
}
const patch::FlatPatchType& patchType() {
    static const patch::FlatPatchType p("eq", 1, 0, table(), false);
    return p;
}
std::string describe(const Values& v, patch::Detail detail) {
    const auto d = defaults();
    std::string s;
    for (std::size_t b = 0; b < kBands; ++b) {
        const auto i = b * kBandParams;
        bool changed = false;
        for (std::size_t f = 0; f < kBandParams; ++f) changed |= v[i + f] != d[i + f];
        if (detail == patch::Detail::Changed && !changed) continue;
        std::string piece = "band " + std::to_string(b + 1) + " ";
        if (v[i + Enabled] < 0.5)
            piece += "off";
        else {
            piece += std::string(types[static_cast<std::size_t>(v[i + Type])].label) + " " +
                     params::format(table()[i + Freq], v[i + Freq]);
            const int t = static_cast<int>(v[i + Type]);
            if (t == HighPass || t == LowPass)
                piece += " " + std::string(slopes[static_cast<std::size_t>(v[i + Slope])].label);
            else if (t == Bell || t == LowShelf || t == HighShelf || t == Tilt)
                piece += " " + params::format(table()[i + Gain], v[i + Gain]);
            piece += " Q " + params::format(table()[i + Q], v[i + Q]);
            if (static_cast<int>(v[i + Channel]) != Stereo)
                piece += " (" +
                         std::string(channels[static_cast<std::size_t>(v[i + Channel])].key) + ")";
        }
        if (detail == patch::Detail::Changed && s.size() + piece.size() > 230) {
            s += "; more bands";
            break;
        }
        if (!s.empty()) s += " → ";
        s += piece;
    }
    for (auto i : {kPhase, kAutoGain, kOutput})
        if (detail == patch::Detail::Full || v[i] != d[i]) {
            if (!s.empty()) s += "; ";
            s += std::string(table()[i].key) + " " + params::format(table()[i], v[i]);
        }
    return s.empty() ? "default curve" : s;
}
}  // namespace supple::eq
