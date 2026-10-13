// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The runtime's side of a running script, shared by the glue that drives a
// script (the player's JIT path and a source export's compiled path) and by
// the script compiler, which reads these layouts. Scripts never see it.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "supple/script.h"

namespace supple::script::detail {

// What a script's code finds wrong while it runs, counted per kind, with the
// source line of the first occurrence (0 when the export path cannot know).
struct Status {
    std::atomic<std::uint32_t> outOfRange{0};
    std::atomic<std::uint32_t> outOfRangeLine{0};
    std::atomic<std::uint32_t> budgetExceeded{0};
    std::atomic<std::uint32_t> budgetLine{0};
};

// An instrument's voices as the glue keeps them (the framework's voice
// manager): what Instrument's methods read and do. Indexes are in range.
class VoiceBank {
  public:
    virtual int count() const = 0;
    virtual bool playing(int v) const = 0;
    virtual int key(int v) const = 0;
    virtual int channel(int v) const = 0;
    virtual double velocity(int v) const = 0;
    virtual double pitch(int v, int frame) const = 0;
    virtual void finished(int v) = 0;
    virtual void setMode(VoiceMode mode) = 0;
    virtual void setGlide(double ms) = 0;
    virtual void setBendRange(double semitones) = 0;

  protected:
    ~VoiceBank() = default;
};

// One running plugin instance as the runtime sees it. The plugin struct's
// Plugin base points here; built-in objects reach it through current().
struct Instance {
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    int channels = 2;
    bool offline = false;
    // Back-edges a hook may take before it is stopped (audio hooks only).
    std::int64_t budget = std::int64_t{1} << 31;
    Status status;
    VoiceBank* voices = nullptr;  // an instrument's

    static void bind(Plugin& plugin, Instance* instance) { plugin.instance_ = instance; }
};

// The instance whose hook is running on this thread. The glue sets it around
// every call into a script (CurrentScope); built-in objects read the sample
// rate from it.
Instance* current();
void setCurrent(Instance* instance);

class CurrentScope {
  public:
    explicit CurrentScope(Instance* instance) : previous_(current()) { setCurrent(instance); }
    ~CurrentScope() { setCurrent(previous_); }
    CurrentScope(const CurrentScope&) = delete;
    CurrentScope& operator=(const CurrentScope&) = delete;

  private:
    Instance* previous_;
};

// Reports an out-of-range index at `line` (0: unknown) to the current instance.
void outOfRange(int line);

// Where the runtime classes keep what generated code reads directly.
struct Layout {
    static constexpr int paramValue = offsetof(Param, value_);
    static constexpr int paramGlide = offsetof(Param, glide_);
    static constexpr int paramGlideFrames = offsetof(Param, glideFrames_);
    static constexpr int paramChanged = offsetof(Param, changed_);
    static constexpr int paramReported = offsetof(Param, reported_);
    static constexpr int pluginInstance = offsetof(Plugin, instance_);
    template <class T>
    static constexpr int bufferItems() {
        return offsetof(Buffer<T>, items_);
    }
    template <class T>
    static constexpr int bufferSize() {
        return offsetof(Buffer<T>, size_);
    }
    template <class T>
    static constexpr int bufferSpare() {
        return offsetof(Buffer<T>, spare_);
    }
};

// How the glue reaches a Param's fields.
struct ParamBinding {
    static void setValue(Param& p, double value, bool changed) {
        p.value_ = value;
        p.changed_ = changed;
    }
    static void setGlide(Param& p, const double* glide, int frames) {
        p.glide_ = glide;
        p.glideFrames_ = frames;
    }
    static void setRange(Param& p, double min, bool offAtMinimum) {
        p.min_ = min;
        p.offAtMinimum_ = offAtMinimum;
    }
    static double reported(const Param& p) { return p.reported_; }
    static int slot(const Param& p) { return p.slot_; }
    static void setSlot(Param& p, int slot) { p.slot_ = slot; }
};

// Collects the parameters a plugin struct declares while it is constructed
// (a source export: the C++ compiler runs the member initializers). The glue
// installs one around the construction; Param's constructor reports to it.
class ParamRegistrar {
  public:
    virtual ~ParamRegistrar() = default;
    virtual void declare(Param& param, const ParamSpec& spec) = 0;
};
ParamRegistrar* registrar();
void setRegistrar(ParamRegistrar* registrar);

}  // namespace supple::script::detail
