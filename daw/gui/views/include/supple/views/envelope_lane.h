// Automation lanes (gui.md §4.3, P5.14): how an envelope's values map to a
// lane's height and read as text. A track's volume sits on the fader's taper
// (so the lane and the fader agree), pan is centred, mute is off/on. Plugin
// parameter lanes take their range and curve from the plugin's description
// (the app supplies it, see TrackLayout::pluginScales).
#pragma once

#include <optional>
#include <string>

#include <memory>

#include "supple/model/session.h"
#include "supple/params/params.h"

namespace supple::views {

struct EnvScale {
    enum class Kind { Volume, Pan, Mute, Plugin };
    Kind kind = Kind::Volume;
    std::shared_ptr<const params::DescribedParam> param;  // Plugin: range, curve, units
    double toNorm(double value) const;                    // 0 = the lane's bottom, 1 = its top
    double fromNorm(double norm) const;                   // the inverse, clamped to the range
    std::string text(double value) const;                 // "-6.0 dB", "L 30", "muted", "1.20 kHz"
    double reference() const;  // the lane's guide line: unity, centre, default
};

// The scale of a track's (or route's) mixer envelope; nullopt for others.
std::optional<EnvScale> envScale(const model::Envelope& env);
// "Volume", "Pan", "Mute" (or the parameter key).
std::string envTitle(const model::Envelope& env);
// The envelope's value at a beat (points are in beats), with the segment
// shapes the engine plays; `fallback` when it has no points.
double envValueAt(const model::Envelope& env, double beat, double fallback);
// The order lanes appear under their track: volume, pan, mute, then others.
int envOrder(const model::Envelope& env);

}  // namespace supple::views
