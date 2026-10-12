// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The contract between the framework and a product's DSP.
//
// product::Processor derives from ProcessorBase<Processor> and provides:
//
//   void prepare(const ProcessSetup&, const ParamValues&);   // allocate, size, build
//   void update(const ParamValues&);                         // parameter targets
//   void process(Block&);                                    // in-place audio
//
// and may shadow any of the defaults below: note() and the other note hooks
// for a product with note input, latency and tail, telemetry, voices. The framework guarantees the
// order: prepare, then update with values.initial() true (snap smoothers to
// the restored session, no ramp from constructor defaults), then any number
// of process/update calls, each update carrying only what changed. reset()
// is likewise followed by an initial update.
//
// Threads. prepare runs on the host's main thread while audio is stopped;
// everything else runs on the audio thread. The processor never sees another
// thread: parameter changes from the host, the editor and state loading all
// arrive through update() on the audio thread. Nothing here may allocate,
// lock, log or touch files outside prepare().
//
// Audio is always double precision and always in place. For an effect,
// Block holds the input on entry and must hold the output on return; for an
// instrument it arrives zeroed. Blocks are split at parameter, note and
// telemetry boundaries, so a block may be as short as one sample: output must
// not depend on where the splits fall.
//
// Bypass is not the processor's business. A parameter marked bypass() is
// handled by the framework, crossfading to the input delayed by the reported
// latency, while the processor keeps running so un-bypassing is seamless.
//
// Parameters marked smoothed(ms) arrive per sample in Block::smoothed(id),
// glided by the framework; output parameters are reported with setOutput().
//
// Buses a product switches on in product.cmake arrive in the same Block: a
// sidechain (PRODUCT_SIDECHAIN) to read, aux outputs (PRODUCT_AUX_OUTPUTS) to
// write. Notes and MIDI (PRODUCT_NOTE_INPUT, and every instrument and note
// effect) arrive through note(), midi(), noteExpression() and
// noteModulation(), each at its exact sample between splits; notes and MIDI
// go out (PRODUCT_NOTE_OUTPUT) through sendNote() and sendMidi().
//
// What the host says about the session reaches the processor too: whether it
// is rendering offline (kOfflineMode), the names of its keys (noteNames), and
// microtuning (PRODUCT_TUNING, retuning()).
#pragma once

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/Definition.h"
#include "core/Files.h"
#include "core/Layout.h"
#include "core/Notes.h"
#include "core/ParamValues.h"
#include "core/Tuning.h"
#include "dsp/AudioFile.h"
#include "ProductInfo.h"
#if !SUPPLE_PRODUCT_RUNTIME
// A product with tables reads them as constants too (params::find, ...).
#include "core/ParamTable.h"
#endif

namespace supple {

struct ProcessSetup {
    double sampleRate = 48000.0;
    int maxBlockSize = 512;  // no Block is longer than this
    int channels = 2;        // main bus width, fixed until the next prepare; 0 for a note effect
    Layout layout = Layout::Stereo;  // which channel is which speaker (core/Layout.h)
    // The host renders without real-time pressure (a bounce), for a processor
    // that declares kOfflineMode. It may change while active: block.offline
    // says what holds now.
    bool offline = false;
};

// Host transport at the start of the current Block, when the host sent one.
// The framework advances `beats` to each split's first sample while playing
// with a tempo; `barStart` and the rest are as the host block began. The
// tempo holds for the whole host block: a ramp within it (CLAP's tempo_inc)
// and a transport event in mid-block (a jump, a stop) are seen from the
// next block, at most one block late.
struct Transport {
    bool playing = false;
    bool hasTempo = false;
    double tempo = 120.0;  // beats per minute
    bool hasBeats = false;
    double beats = 0.0;     // song position in quarter notes
    double barStart = 0.0;  // in quarter notes
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
};

struct Block {
    double* const* data = nullptr;  // data[channel][frame]
    int channels = 0;
    int frames = 0;
    // True while an editor is open. Accumulate whatever captureTelemetry()
    // reports only when this is set; nobody is looking otherwise.
    bool metering = false;
    const Transport* transport = nullptr;  // null when the host sent none
    // Rendering offline, for a processor that declares kOfflineMode: spend
    // what the better algorithm costs.
    bool offline = false;

    double* channel(int c) const { return data[c]; }

    // PRODUCT_SIDECHAIN: the sidechain input, sidechain[channel][frame],
    // `channels` wide. Silence when the host connected nothing to it, which
    // sidechainConnected says.
    const double* const* sidechain = nullptr;
    bool sidechainConnected = false;
    // PRODUCT_AUX_OUTPUTS: extra output bus `bus` (in product.cmake's order),
    // aux(bus)[channel][frame], `channels` wide and zero on entry. What is
    // written leaves with the main output's latency; bypass silences it.
    double* const* aux(int bus) const { return auxData[bus]; }
    int auxOutputs = 0;
    double* const* const* auxData = nullptr;

    // A parameter marked smoothed(ms), in plain units, at each of this
    // block's frames: gliding to every change (sample-accurately, wherever
    // the host's blocks split) and exactly the target once it arrives.
    const double* smoothed(uint32_t id) const { return glides[slot(id)]; }
    // False when smoothed(id) holds one value throughout this block, so a
    // processor can do its per-sample conversions once.
    bool gliding(uint32_t id) const { return moving[slot(id)]; }
    // The host's tempo, or `fallback` when it sent none.
    double tempo(double fallback = 120.0) const {
        return transport && transport->hasTempo && transport->tempo > 0.0 ? transport->tempo : fallback;
    }

    // The engine's.
    const ParamTable* params = nullptr;
    const double* const* glides = nullptr;
    const bool* moving = nullptr;

  private:
    size_t slot(uint32_t id) const {
        const int i = params->indexOf(id);
        const int k = i >= 0 ? params->smoothedSlot(static_cast<size_t>(i)) : -1;
        assert(k >= 0 && "Block::smoothed reads a parameter marked smoothed()");
        return k >= 0 ? static_cast<size_t>(k) : 0;
    }
};

// Seconds in a number of beats (quarter notes) at a tempo.
inline double beatsToSeconds(double beats, double bpm) { return beats * 60.0 / bpm; }

// Per-note modulation of a parameter marked polyModulatable(): `amount` is
// an offset in host units for the notes `address` names, on top of the
// parameter's value. ParamValues::modulated(id, amount) gives the plain
// value it makes. A voice starts unmodulated and keeps the latest amount.
struct NoteModulation {
    NoteAddress address;
    uint32_t id = 0;
    double amount = 0.0;
};

// What the processor sends the host during one host block, in time order:
// note ends (CLAP NOTE_END) for notes it was given, and with
// PRODUCT_NOTE_OUTPUT notes and MIDI of its own. Hosts use note ends for
// their own voice bookkeeping: the VST3 wrapper keeps every note it ever
// started until one arrives, growing a list on the audio thread. Bounded;
// beyond kCapacity in one block the rest are counted, not sent.
class NoteOutput {
  public:
    static constexpr int kCapacity = 1024;
    enum class Kind : uint8_t { End, Note, Midi };
    struct Entry {
        uint32_t time;  // frame within the host block
        Kind kind;
        NoteEvent note;    // End, Note
        MidiMessage midi;  // Midi
    };

    void clear() { count_ = 0; }
    // Kept in time order, as host event lists must be; equal times keep the
    // order they were sent in.
    void push(Kind kind, const NoteEvent& note, const MidiMessage& midi, uint32_t time) {
        if (count_ == kCapacity) {
            ++dropped_;
            return;
        }
        int i = count_++;
        for (; i > 0 && entries_[i - 1].time > time; --i) entries_[i] = entries_[i - 1];
        entries_[i] = Entry{time, kind, note, midi};
    }
    int count() const { return count_; }
    const Entry& operator[](int i) const { return entries_[i]; }
    uint32_t dropped() const { return dropped_; }

    // The engine sets where the processor's frame 0 falls in the host block.
    void setOffset(uint32_t offset) { offset_ = offset; }
    uint32_t offset() const { return offset_; }

  private:
    Entry entries_[kCapacity];
    int count_ = 0;
    uint32_t offset_ = 0;
    uint32_t dropped_ = 0;
};

// Names for keys, which hosts show in piano rolls and drum maps (CLAP
// note-name). Main thread only.
struct NoteName {
    int key = 0;       // 0..127
    int channel = -1;  // -1: every channel
    int port = -1;     // -1: every port
    std::string name;  // up to 255 bytes of UTF-8
};

class NoteNames {
  public:
    void add(int key, std::string_view name, int channel = -1, int port = -1) {
        if (key < 0 || key > 127 || name.empty()) return;
        names_.push_back(NoteName{key, channel, port, std::string(name.substr(0, 255))});
    }
    size_t size() const { return names_.size(); }
    const NoteName& operator[](size_t i) const { return names_[i]; }
    bool operator==(const NoteNames& o) const {
        if (names_.size() != o.names_.size()) return false;
        for (size_t i = 0; i < names_.size(); ++i) {
            const NoteName &a = names_[i], &b = o.names_[i];
            if (a.key != b.key || a.channel != b.channel || a.port != b.port || a.name != b.name) return false;
        }
        return true;
    }

  private:
    std::vector<NoteName> names_;
};

// Whether a processor names its keys (ProcessorBase: noteNames).
template <class P>
concept NamesNotes = requires(const ParamValues& v, NoteNames& n) { P::noteNames(v, n); };

// P's note names, or none.
template <class P>
void noteNamesOf(const ParamValues& v, NoteNames& names) {
    if constexpr (NamesNotes<P>) P::noteNames(v, names);
}

struct NoTelemetry {};

template <class Derived>
class ProcessorBase {
  public:
    // Voices, for hosts that plan around them (CLAP voice-info): an
    // instrument declares its pool's size, and voiceCount() how many of them
    // the current settings can use (fewer in a mono mode). It is read after
    // every update(). kOverlappingNotes: two notes on one key can sound at
    // once, told apart by note ID.
    static constexpr int kVoices = 0;
    static constexpr bool kOverlappingNotes = false;
    int voiceCount() const { return Derived::kVoices; }

    // What the processor publishes to its editor at kTelemetryHz, beyond the
    // input and output peak meters the framework measures itself. A product
    // declares its own `struct Telemetry` to replace this.
    using Telemetry = NoTelemetry;
    // Frames per second sent to an open editor. Choose it from what the
    // display needs: meters are fine at 60; a scrolling envelope of a fast
    // transient may need several hundred.
    static constexpr double kTelemetryHz = 120.0;
    // log2 of the telemetry queue length. Lower it when Telemetry is large
    // (spectra, waveforms): the queue holds this many whole frames.
    static constexpr int kTelemetryQueueLog2 = 9;

    // A processor that renders differently offline -- a costlier, better
    // algorithm for bounces -- says so here. Hosts then tell it when they
    // render without real-time pressure (CLAP render; VST3's offline process
    // mode), which setup.offline and block.offline report. Others never ask.
    static constexpr bool kOfflineMode = false;

    // An editor that draws the audio itself -- a scope, a spectrum -- says
    // so here, and while it is open the framework copies the main input and
    // output to it (core/AudioFeed.h, EditorBase::audio()). Others pay
    // nothing.
    static constexpr bool kAudioFeed = false;

    // Reported to the host at activation. If it changes while active the
    // framework asks the host to restart; until then the old value stands.
    int latencySamples() const { return 0; }
    // The longest latencySamples() can become before the next prepare(), so
    // the bypass delay is sized once. Override when latency follows a
    // parameter.
    int maxLatencySamples() const { return static_cast<const Derived&>(*this).latencySamples(); }
    // Samples of output after the input falls silent; UINT32_MAX for infinite.
    uint32_t tailSamples() const { return 0; }

    void reset() {}
    // Note input. A note's release or choke names the notes it ends by
    // address (NoteAddress::of(e).matches(voice)).
    void note(const NoteEvent&) {}
    void midi(const MidiMessage&) {}
    void noteExpression(const NoteExpression&) {}
    void noteModulation(const NoteModulation&) {}
    // A property's value (product/Properties.h): every one after prepare(),
    // then each as it changes, before the block that follows. The view stays
    // valid until that property's next delivery; copy or parse what is
    // needed, without allocating.
    void property(uint32_t id, std::string_view value) { (void)id, (void)value; }

    // File properties (.file(), .audioFile()): what a file becomes for
    // process(), made on the framework's loading thread -- never the audio
    // thread -- whenever the property's path changes, and again when the
    // sample rate does. The default decodes audio (WAV, AIFF, FLAC) and
    // resamples it to the session's rate. A processor wanting something else
    // -- a convolution kernel, a parsed scale -- declares its own:
    //
    //   using FileContents = Impulse;
    //   static std::unique_ptr<Impulse> loadFile(const supple::FileRequest& r, std::string& error);
    //
    // returning null, with a reason in `error`, for a file it cannot use.
    // Its own can start from this one, with `maxSeconds` (above zero) to
    // decode only the start of a long file: an impulse response's ten
    // seconds, not an hour's recording.
    using FileContents = dsp::AudioData;
    static std::unique_ptr<dsp::AudioData> loadFile(const FileRequest& request, std::string& error,
                                                    double maxSeconds = 0.0) {
        auto audio = std::make_unique<dsp::AudioData>();
        if (!dsp::loadAudioFile(request.path, *audio, &error, maxSeconds)) return nullptr;
        if (audio->sampleRate != request.sampleRate) *audio = dsp::resample(*audio, request.sampleRate);
        return audio;
    }
    // On the audio thread, at the start of a block and after prepare(): a
    // file property's contents as loadFile() made them, or null -- no file,
    // a missing one, one that failed. The processor's alone, state and all
    // (a convolution runs in it), until the next call for that id; the
    // framework frees it, later, elsewhere.
    void file(uint32_t id, dsp::AudioData* contents) { (void)id, (void)contents; }

    // Names for keys (CLAP note-name), for a product with note input: a drum
    // map's "Kick" on 36. A processor that has them defines
    //
    //   static void noteNames(const supple::ParamValues& v, supple::NoteNames& names);
    //
    // called on the main thread with the current parameter values, whenever a
    // host asks (NamesNotes says whether it does). Names that follow a
    // parameter call noteNamesChanged() from update() as it changes.
    //
    // The names noteNames() gives have changed; hosts are told to read them
    // again. From update() or process().
    void noteNamesChanged() {
        if (namesChanged_) *namesChanged_ = true;
    }
    // The engine's; a processor never calls it.
    void bindNoteNamesChanged(bool* flag) { namesChanged_ = flag; }

    // PRODUCT_TUNING: how far `key` on MIDI `channel` sits from equal
    // temperament at A4 = 440 Hz, in semitones (core/Tuning.h), at `frame`
    // of the current Block. 0 without a tuning. From process() or note().
    double retuning(int key, int channel, int frame = 0) const {
        if (!tuning_) return 0.0;
        const uint32_t at = (noteOutput_ ? noteOutput_->offset() : 0) + static_cast<uint32_t>(frame < 0 ? 0 : frame);
        return tuning_->retuning(key, channel, at);
    }
    // The engine's; a processor never calls it.
    void bindTuning(TuningSource* source) { tuning_ = source; }

    // Reports an output parameter's value, in plain units, from update() or
    // process(). The framework passes it to the host (about 30 times a
    // second, when it changed) and to the editor.
    void setOutput(uint32_t id, double plain) {
        const int i = outputTable_ ? outputTable_->indexOf(id) : -1;
        assert(i >= 0 && outputTable_->at(static_cast<size_t>(i)).isOutput() && "setOutput needs an output() parameter");
        if (outputs_ && i >= 0 && std::isfinite(plain)) outputs_[i] = plain;
    }
    // The engine's; a processor never calls it.
    void bindOutputs(double* sink, const ParamTable* table) {
        outputs_ = sink;
        outputTable_ = table;
    }

    // A product with note input calls this when it is done with a note it was
    // given -- a voice released to silence, choked, stolen, reset; a note
    // effect when the note is released: `frame` is within the current Block
    // in process(), and 0 inside note(). See NoteOutput.
    void noteEnded(const NoteEvent& address, int frame = 0) { emit(NoteOutput::Kind::End, address, {}, frame); }
    // PRODUCT_NOTE_OUTPUT: a note on, off or choke to the host's next plugin,
    // and a MIDI message, at `frame` as above.
    void sendNote(const NoteEvent& note, int frame = 0) {
        assert(sendsNotes_ && "sendNote needs PRODUCT_NOTE_OUTPUT");
        emit(NoteOutput::Kind::Note, note, {}, frame);
    }
    void sendMidi(const MidiMessage& message, int frame = 0) {
        assert(sendsNotes_ && "sendMidi needs PRODUCT_NOTE_OUTPUT");
        emit(NoteOutput::Kind::Midi, {}, message, frame);
    }
    // The engine's; a processor never calls it.
    void bindNoteOutput(NoteOutput* sink, bool sendsNotes) {
        noteOutput_ = sink;
        sendsNotes_ = sendsNotes;
    }
    template <class T>
    void captureTelemetry(T&) {}

    // Called on the main thread when saved state did not come from this
    // framework -- a session from before a product moved onto it. Read the
    // old format and report values through `set(id, plainValue)`; return
    // false to reject the data.
    template <class Setter>
    static bool loadLegacyState(const uint8_t* data, size_t size, Setter&& set) {
        (void)data;
        (void)size;
        (void)set;
        return false;
    }

  private:
    void emit(NoteOutput::Kind kind, const NoteEvent& note, const MidiMessage& midi, int frame) {
        if (noteOutput_)
            noteOutput_->push(kind, note, midi, noteOutput_->offset() + static_cast<uint32_t>(frame < 0 ? 0 : frame));
    }

    NoteOutput* noteOutput_ = nullptr;
    bool sendsNotes_ = false;
    double* outputs_ = nullptr;
    const ParamTable* outputTable_ = nullptr;
    bool* namesChanged_ = nullptr;
    TuningSource* tuning_ = nullptr;
};

}  // namespace supple
