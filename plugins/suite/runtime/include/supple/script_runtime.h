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
    std::atomic<bool> silent{false};

    // The hook-owning thread clears observations when code is replaced/reset.
    void clear() {
        silent.store(false, std::memory_order_relaxed);
        outOfRange.store(0, std::memory_order_relaxed);
        outOfRangeLine.store(0, std::memory_order_relaxed);
        budgetExceeded.store(0, std::memory_order_relaxed);
        budgetLine.store(0, std::memory_order_relaxed);
    }
};
static_assert(std::atomic<bool>::is_always_lock_free);

// An instrument's voices as the glue keeps them (the framework's voice
// manager): what Instrument's methods read and do. Indexes are in range.
class VoiceBank {
  public:
    virtual int count() const = 0;
    virtual bool playing(int v) const = 0;
    virtual int key(int v) const = 0;
    virtual int channel(int v) const = 0;
    virtual double velocity(int v) const = 0;
    virtual double pitch(int v, int frame) const = 0;  // its tuning expression included
    // Expression `which` of the voice's note, in CLAP's order: volume, pan,
    // tuning, vibrato, expression, brightness, pressure.
    virtual double expression(int v, int which) const = 0;
    // Parameter table index `slot`'s `value` with voice v's modulation.
    virtual double modulated(int slot, int v, double value) const = 0;
    virtual void finished(int v) = 0;
    virtual void setMode(VoiceMode mode) = 0;
    virtual void setGlide(double ms) = 0;
    virtual void setBendRange(double semitones) = 0;

  protected:
    ~VoiceBank() = default;
};

// What editor code reaches through its instance: the glue's script editor
// implements it, on the main thread.
class EditorServices {
  public:
    // Room for `bytes` of text that lasts until the hook returns; null when
    // the hook has used all there is.
    virtual char* text(int bytes) = 0;
    // Parameter table index `slot`'s value as the host shows it, and its name.
    virtual const char* paramText(int slot, double value) = 0;
    virtual const char* paramName(int slot) = 0;
    // Where `value` sits from 0 to 1 on its knob.
    virtual double paramNormalized(int slot, double value) = 0;
    // pointer()'s edits (Param::edit, editNormalized, reset): the parameter
    // to a value in its units, a position from 0 to 1, or its default.
    // Each sets the Param's value in the editor's copy as well.
    enum class Edit { Value, Position, Default };
    virtual void editParam(int slot, Edit kind, double value) = 0;

  protected:
    ~EditorServices() = default;
};

// What a Canvas draws with (the glue's, over the framework's renderer). Every
// call is script.h's Canvas method of the same name; colours are as there.
class Painter {
  public:
    virtual double width() const = 0;
    virtual double height() const = 0;
    virtual void save() = 0;
    virtual void restore() = 0;
    virtual void translate(double x, double y) = 0;
    virtual void rotate(double radians) = 0;
    virtual void scale(double x, double y) = 0;
    virtual void fillStyle(int colour) = 0;
    virtual void strokeStyle(int colour) = 0;
    virtual void globalAlpha(double alpha) = 0;
    virtual void lineWidth(double width) = 0;
    virtual void lineCap(LineCap cap) = 0;
    virtual void lineJoin(LineJoin join) = 0;
    virtual void linearGradient(double x0, double y0, double x1, double y1, int from, int to) = 0;
    virtual void radialGradient(double x, double y, double inner, double outer, int from,
                                int to) = 0;
    virtual void fillRect(double x, double y, double w, double h) = 0;
    virtual void strokeRect(double x, double y, double w, double h) = 0;
    virtual void clearRect(double x, double y, double w, double h) = 0;
    virtual void beginPath() = 0;
    virtual void closePath() = 0;
    virtual void moveTo(double x, double y) = 0;
    virtual void lineTo(double x, double y) = 0;
    virtual void bezierCurveTo(double c1x, double c1y, double c2x, double c2y, double x,
                               double y) = 0;
    virtual void quadraticCurveTo(double cx, double cy, double x, double y) = 0;
    virtual void arc(double x, double y, double radius, double start, double end) = 0;
    virtual void arcTo(double x1, double y1, double x2, double y2, double radius) = 0;
    virtual void rect(double x, double y, double w, double h) = 0;
    virtual void roundRect(double x, double y, double w, double h, double radius) = 0;
    virtual void ellipse(double x, double y, double rx, double ry) = 0;
    virtual void circle(double x, double y, double radius) = 0;
    virtual void fill() = 0;
    virtual void stroke() = 0;
    virtual void clip(double x, double y, double w, double h) = 0;
    enum class Face { Regular, Bold, Mono, Serif, SerifItalic };
    virtual void font(Face face, double size) = 0;
    virtual void textAlign(TextAlign align) = 0;
    virtual void textBaseline(TextBaseline baseline) = 0;
    virtual void fillText(const char* text, double x, double y) = 0;
    virtual double measureText(const char* text) = 0;
    virtual void panel(double x, double y, double w, double h, const char* title) = 0;
    virtual void well(double x, double y, double w, double h) = 0;
    virtual void meter(double db, double x, double y, double w, double h) = 0;
    virtual void frequencyGrid(double x, double y, double w, double h, double lowHz,
                               double highHz) = 0;
    virtual void levelGrid(double x, double y, double w, double h, double lowDb, double highDb,
                           double stepDb) = 0;

  protected:
    ~Painter() = default;
};

// What an Editor builds with (the glue's). Parameters are by table index
// (Param's slot); a file by its member's address in the plugin struct.
class EditorBuilder {
  public:
    enum class Control { Knob, Slider, Toggle, Choice, Readout };
    virtual void size(double width, double height) = 0;
    virtual void control(Control kind, int slot, double x, double y, double w, double h) = 0;
    virtual void xyPad(int across, int up, double x, double y, double w, double h) = 0;
    virtual void envelope(int attack, int decay, int sustain, int release, double x, double y,
                          double w, double h) = 0;
    virtual void file(const void* member, double x, double y, double w, double h) = 0;
    virtual void keyboard(int lowKey, int highKey, double x, double y, double w, double h) = 0;
    virtual void meters(double x, double y, double w, double h) = 0;

  protected:
    ~EditorBuilder() = default;
};

// Only used while a native plugin's metadata is collected, never by hooks.
class PatchBuilder {
  public:
    virtual void add(const char* name) = 0;
    virtual void set(const Param& param, double physical) = 0;
    virtual void set(const Param& param, const char* choiceKey) = 0;
    virtual ~PatchBuilder() = default;
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
    VoiceBank* voices = nullptr;       // an instrument's
    int latency = -1;                  // setLatency()'s, or -1: Info's
    EditorServices* editor = nullptr;  // the editor's copy of the plugin: text, names

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
    // The glue's: a Canvas, Editor or Pointer made for a hook.
    static void bind(Canvas& canvas, Painter* painter) { canvas.painter_ = painter; }
    static void bind(Editor& editor, EditorBuilder* builder) { editor.builder_ = builder; }
    static void set(Pointer& p, PointerAction action, double x, double y, double dx, double dy,
                    double wheel, bool doubleClick, bool shift, bool ctrl, bool alt) {
        p.action_ = action;
        p.x_ = x;
        p.y_ = y;
        p.dx_ = dx;
        p.dy_ = dy;
        p.wheel_ = wheel;
        p.doubleClick_ = doubleClick;
        p.shift_ = shift;
        p.ctrl_ = ctrl;
        p.alt_ = alt;
    }
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

// How the glue gives an AudioFile or Convolver member its file (or none):
// both hold only the pointer, so the glue needs no more than the member's
// offset.
struct FileBinding {
    static void bind(void* member, FileContents* contents) {
        *static_cast<FileContents**>(member) = contents;
    }
    static_assert(sizeof(AudioFile) == sizeof(FileContents*) &&
                  sizeof(Convolver) == sizeof(FileContents*));
    static constexpr int audioFileContents() { return offsetof(AudioFile, contents_); }
    static constexpr int convolverContents() { return offsetof(Convolver, contents_); }
};
static_assert(FileBinding::audioFileContents() == 0 && FileBinding::convolverContents() == 0);

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
    // An AudioFile or Convolver member (`member` points at it).
    virtual void declareFile(void* member, const FileSpec& spec) { (void)member, (void)spec; }
    // A Shared or SharedArray member, `bytes` long.
    virtual void declareShared(void* member, int bytes) { (void)member, (void)bytes; }
};
ParamRegistrar* registrar();
void setRegistrar(ParamRegistrar* registrar);

}  // namespace supple::script::detail
