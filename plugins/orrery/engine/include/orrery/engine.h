// Orrery's real-time synth engine (spec: docs/specs/synth.md §2, §5.4).
//
// Threads: prepare()/setPatch()/collect() on the main thread (not concurrently
// with each other); process()/reset() on the audio thread. setPatch() compiles
// the patch into an immutable Program and hands it over wait-free
// (RtHandoff); the audio thread adopts it at the start of a block — values
// glide to the new ones (the parameters' smoothing times), changed slot types
// restart that slot in sounding voices, a quality change restarts all voices.
//
// Determinism: all randomness derives from the patch seed and the note count.
// Renders do not depend on the host block size: events apply at their sample
// time, modulation runs on a control grid anchored at each voice's start
// (global modulators and FX parameters at absolute sample multiples of 16).
#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "orrery/patch.h"
#include "supple/base/rt.h"

namespace supple::orrery {

struct Event {
    enum class Type : std::uint8_t {
        NoteOn,      // key, channel, noteId, value = velocity 0..1
        NoteOff,     // key, channel, noteId, value = release velocity 0..1
        Choke,       // key, channel, noteId: conjunctive match, -1 wildcards
        Param,       // index = table index, value = physical value
        Controller,  // index = MIDI CC number, value 0..1 (1 modwheel, 2 breath, 11 expression, 74
                     // slide, 64 sustain)
        Pitchbend,   // channel, value -1..1
        Pressure,    // channel aftertouch 0..1 (MPE member channel: per-note pressure)
        PolyPressure,  // key, channel, noteId (-1 wildcards), value 0..1
        AllNotesOff,
        NoteExpression,  // key/channel/noteId (-1 wildcards), index = 11 expression / 74 slide
                         // value 0..1; separate from global MIDI controllers
    };
    std::uint32_t time = 0;  // sample offset in the block; events are sorted by time
    Type type = Type::NoteOn;
    std::int16_t key = -1;
    std::int16_t channel = 0;
    std::int32_t noteId = -1;
    std::uint32_t index = 0;
    double value = 0.0;
};

struct Transport {
    double tempo = 120.0;  // BPM, for synced LFOs, steps and delays
};

class Engine {
  public:
    static constexpr int kMaxVoices = 64;  // sounding voices (notes × unison)

    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Main thread, while not processing. Allocates everything the audio
    // thread will use; sample rates 22.05..192 kHz, any block size ≤ maxBlock.
    void prepare(double sampleRate, std::uint32_t maxBlock);
    double sampleRate() const { return sampleRate_; }

    // Main thread; wait-free for the audio thread. Allocates (compiles).
    void setPatch(const Patch& patch);
    // Main thread: frees programs the audio thread no longer uses.
    void collect();

    // Audio thread. Silence, all voices off, modulators to their start.
    void reset() SUPPLE_RT;
    // Audio thread. Writes `frames` samples (≤ maxBlock) to left/right.
    void process(std::span<const Event> events, const Transport& transport, float* left,
                 float* right, std::uint32_t frames) SUPPLE_RT;
    // Audio thread, while active but not processing (the audio-thread half of
    // clap_plugin_params.flush, which cannot compile a program): the values the
    // plugin's store holds now, one per table parameter. A running engine glides
    // to them like any other parameter move; after prepare()/reset() the next
    // block adopts them and snaps, with a fresh instance's seeding.
    void setValues(std::span<const double> values) SUPPLE_RT;

    // Audio thread (or when stopped): voices sounding, and whether the output
    // is silent with no voices and no FX tail (the plugin may sleep).
    int activeVoices() const SUPPLE_RT;
    bool silent() const SUPPLE_RT;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    double sampleRate_ = 0.0;
};

}  // namespace supple::orrery
