// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What an editor may ask of the plugin. The CLAP plugin implements it; tests
// and the preview tool implement it with plain data.
//
// Values are host values (see core/Param.h). Editors normally go through Ui,
// which converts, gestures and clamps; draw-only code reads value() and
// converts with toPlain().
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/AudioFeed.h"
#include "core/Files.h"
#include "core/Telemetry.h"
#include "gui/Menu.h"

namespace supple {
class PresetLibrary;
}  // namespace supple

namespace supple::gui {

// The track the plugin sits on, as far as the host says (CLAP track-info;
// VST3 channel context). Everything is optional.
struct TrackInfo {
    std::string name;  // empty when the host gave none
    bool hasColour = false;
    uint8_t red = 0, green = 0, blue = 0, alpha = 255;
    bool returnTrack = false, bus = false, master = false;
};

// A host's own items for a parameter's context menu (automation, MIDI
// learn...), flattened: a submenu becomes a header and its items. actions[i]
// is what performing items[i] asks of the host.
struct HostMenu {
    std::vector<MenuItem> items;
    std::vector<uint32_t> actions;
};

enum class LogLevel : uint8_t { Debug, Info, Warning, Error };

class EditorHost {
  public:
    virtual ~EditorHost() = default;

    virtual double value(uint32_t id) const = 0;
    // Non-destructive host modulation currently applied, in host units.
    virtual double modulation(uint32_t id) const {
        (void)id;
        return 0.0;
    }

    // One gesture: beginEdit, any number of setValue, endEdit. Ui never has
    // more than one open; the plugin tolerates it anyway.
    virtual void beginEdit(uint32_t id) = 0;
    virtual void setValue(uint32_t id, double host) = 0;
    virtual void endEdit(uint32_t id) = 0;

    // Properties (core/Property.h), by ID. Setting one is a whole edit: no
    // gesture. False when the value is too long for it.
    virtual std::string_view property(uint32_t id) const {
        (void)id;
        return {};
    }
    virtual bool setProperty(uint32_t id, std::string_view value) {
        (void)id, (void)value;
        return false;
    }
    // Changes whenever any property does, so an editor knows to repaint.
    virtual uint64_t propertiesVersion() const { return 0; }
    // What became of a file() property's file: loading, ready, missing.
    virtual FileStatus fileStatus(uint32_t id) const {
        (void)id;
        return {};
    }

    // The preset library, for a product with presets (core/PresetLibrary.h).
    virtual PresetLibrary* presetLibrary() { return nullptr; }

    // The track, and a number that changes whenever it does, so an editor
    // that shows it knows to repaint.
    virtual TrackInfo track() const { return {}; }
    virtual uint64_t trackVersion() const { return 0; }

    // The host's context menu for a parameter's control (CLAP
    // context-menu). hostPopup() asks the host to show its own, which also
    // carries the plugin's items, at (x, y) in design units: false when it
    // cannot, and then hostMenu() collects the host's items for the editor's
    // menu and performHostMenu() runs the one chosen.
    virtual bool hostPopup(uint32_t param, float x, float y) {
        (void)param, (void)x, (void)y;
        return false;
    }
    virtual bool hostMenu(uint32_t param, HostMenu& menu) {
        (void)param, (void)menu;
        return false;
    }
    virtual bool performHostMenu(uint32_t param, uint32_t action) {
        (void)param, (void)action;
        return false;
    }

    // A line in the host's log (CLAP log), or on stderr in a debug build of
    // a host that has none. Never from the audio thread.
    virtual void log(LogLevel level, std::string_view message) { (void)level, (void)message; }

    // Plays a note through the processor's note(), as if the host had sent
    // it: on with a velocity (0..1), or off. Port 0, channel 0, no note ID.
    // The editor's keyboard. Ignored without note input.
    virtual void playNote(int key, double velocity, bool on) { (void)key, (void)velocity, (void)on; }
    // Audio for a product that declared kAudioFeed: up to max frames, oldest
    // first, since the last call. EditorBase drains it.
    virtual size_t readAudio(FeedFrame* out, size_t max) {
        (void)out, (void)max;
        return 0;
    }

    virtual bool popTelemetry(TelemetryFrame& frame) = 0;
    virtual int latencySamples() const = 0;
    virtual double sampleRate() const = 0;
};

}  // namespace supple::gui
