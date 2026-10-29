// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Properties: state that is not a parameter. A custom chord typed as text, an
// instance's label, a drawn curve: anything a host should not automate but a
// session must keep. A product declares them in product/Properties.h, the
// same way as parameters:
//
//   enum PropertyId : uint32_t { kIntervals = 0, kLabel = 1 };
//   inline constexpr supple::PropertyDef kProperties[] = {
//       supple::property(kIntervals, "Intervals", "0 4 7"),
//       supple::property(kLabel, "Label").perProject().maxBytes(64),
//   };
//
// A value is a byte string, normally UTF-8 text; a product that keeps
// something else in one versions its own encoding. IDs are session ABI, like
// parameter IDs. A product without the file has none, and pays nothing.
//
// The editor reads and writes them (EditorHost::property, Ui::textField);
// the processor hears each value on the audio thread through property(),
// after prepare() and whenever it changes. State and presets carry them,
// except that a perProject() property belongs to the session, not to the
// sound: presets leave it out, and loading one leaves it alone.
//
// A property marked file() holds a path (UTF-8), and the framework takes
// care of the file it names (core/Files.h): it relocates one that has moved
// when a session loads, reports one it cannot find as missing, and loads it
// off the audio thread -- by default as audio, decoded and resampled to the
// session's rate -- for the processor's file() hook (core/ProcessorBase.h):
//
//   supple::property(kImpulse, "Impulse").audioFile(),         // WAV, AIFF, FLAC
//   supple::property(kScale, "Scale").file("scl tun"),         // the processor's own loadFile()
//
// Or the product's own file, inside the binary: "factory:plate.flac" names
// one of its PRODUCT_ASSETS (core/Files.h), as a default, in a preset or in a
// session.
//
// The editor shows and picks one with Ui::fileSlot: a dialog, a drop, and
// "Missing" when it is. A product whose files are of several kinds -- a
// sample and an impulse response -- labels each, .ofKind(n), and its
// loadFile() reads the label from the request (FileRequest::kind).
//
// A property marked hidden() is one the automatic editor (gui/AutoEditor.cpp)
// leaves out: text in a format of the product's own, that only its own
// controls or its host edit. State and presets keep it as any other.
#pragma once

#include <cstdint>

namespace supple {

namespace propflag {
inline constexpr uint32_t kPerProject = 1u << 0;
inline constexpr uint32_t kFile = 1u << 1;
inline constexpr uint32_t kHidden = 1u << 2;
}  // namespace propflag

// What audioFile() accepts: what dsp/AudioFile.h decodes.
inline constexpr const char* kAudioFileTypes = "wav wave aif aiff aifc flac rf64 w64";

struct PropertyDef {
    uint32_t id = 0;
    const char* name = "";
    const char* def = "";      // the value a new instance starts with
    uint32_t limit = 4096;     // longest value accepted, in bytes
    uint32_t flags = 0;
    const char* extensions = "";  // file(): what it accepts, space-separated, no dots; "" for anything
    uint32_t kind = 0;            // file(): the product's own label for what loadFile() makes of it
    const char* description = nullptr;  // a sentence of help, as a parameter's

    constexpr bool is(uint32_t flag) const { return (flags & flag) != 0; }
    constexpr bool perProjectOnly() const { return is(propflag::kPerProject); }
    constexpr bool isFile() const { return is(propflag::kFile); }
    constexpr bool isHidden() const { return is(propflag::kHidden); }

    // Kept with the session, left out of presets.
    constexpr PropertyDef perProject() const {
        PropertyDef d = *this;
        d.flags |= propflag::kPerProject;
        return d;
    }
    // A path to a file, loaded for the processor (see above).
    constexpr PropertyDef file(const char* types = "") const {
        PropertyDef d = *this;
        d.flags |= propflag::kFile;
        d.extensions = types;
        return d;
    }
    // Left out of the automatic editor (see above).
    constexpr PropertyDef hidden() const {
        PropertyDef d = *this;
        d.flags |= propflag::kHidden;
        return d;
    }
    // A file of audio, as the default loadFile() decodes it.
    constexpr PropertyDef audioFile() const { return file(kAudioFileTypes); }
    constexpr PropertyDef ofKind(uint32_t label) const {
        PropertyDef d = *this;
        d.kind = label;
        return d;
    }
    constexpr PropertyDef help(const char* text) const {
        PropertyDef d = *this;
        d.description = text;
        return d;
    }
    constexpr PropertyDef maxBytes(uint32_t bytes) const {
        PropertyDef d = *this;
        d.limit = bytes;
        return d;
    }
};

constexpr PropertyDef property(uint32_t id, const char* name, const char* def = "") {
    PropertyDef d;
    d.id = id;
    d.name = name;
    d.def = def;
    return d;
}

}  // namespace supple
