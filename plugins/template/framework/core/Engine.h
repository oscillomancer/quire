// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The audio-thread core, independent of every plugin SDK.
//
// The CLAP plugin, the offline renderer and the tests all drive this one
// class, so what a test measures and what a host plays are the same code.
// It owns:
//
//   - parameter delivery. Host automation, editor edits and loaded state all
//     reach the processor through update() on the audio thread. Other threads
//     only write the ParamStore; nothing but this class touches the processor
//     while it is active.
//   - sample-accurate events. A host block is split exactly where parameter
//     and note events fall, and where a telemetry frame is due, and nowhere
//     else.
//   - precision. Host buffers may be float or double; the processor always
//     sees double, in place.
//   - bypass, crossfaded and delayed by the processor's current latency.
//   - telemetry, at a fixed rate whatever the event density, only while an
//     editor is open.
//   - a non-finite output guard: a NaN or infinity never reaches the host,
//     whether the processor made it, the host fed it in (bypass passes the
//     input), or a finite double overflowed a float buffer. The block is
//     silenced and the processor and dry history reset instead.
//   - buses a product switches on: a sidechain in, aux outputs, each as wide
//     as the main bus; a bus the host left unconnected is silence or
//     discarded.
//   - notes, MIDI, note expressions and per-note modulation in, each at its
//     exact frame; note ends and (PRODUCT_NOTE_OUTPUT) notes and MIDI out
//     (NoteOutput), for the host to collect.
//   - smoothing for parameters marked smoothed(ms), per sample, retargeted
//     at each change's exact frame.
//   - output parameters: what the processor reports with setOutput() goes
//     to the store after every block (the editor and get_value read it) and
//     is due for the host about kOutputHz times a second.
//   - what the host says about the session: offline rendering (setOffline),
//     a tuning (setTuning: unmapped keys never reach the processor), and
//     when the processor's note names changed.
//   - what the editor plays and sees: notes from its keyboard (playNote),
//     the keys held (in telemetry), and the audio itself for a processor
//     that declared kAudioFeed (feed()), each only while an editor is open.
//   - the files file() properties name (setProperty): relocated when they
//     have moved, loaded on a worker thread with the processor's
//     loadFile(), and handed to its file() hook at the start of a block.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "core/AudioFeed.h"
#include "core/Denormals.h"
#include "core/Dsp.h"
#include "core/FileSlots.h"
#include "core/Files.h"
#include "core/ParamTable.h"
#include "core/ParamValues.h"
#include "core/PropertyStore.h"
#include "core/ProcessorBase.h"
#include "core/Realtime.h"
#include "core/Telemetry.h"
#include "core/Tuning.h"
#include "Processor.h"

namespace supple {

// One event for the engine, supplied in time order by whatever drives it.
struct EngineEvent {
    enum class Type : uint8_t { Value, Modulation, Note, Midi, Expression, NoteModulation, Tuning };
    Type type = Type::Value;
    uint32_t time = 0;   // frame offset within the block
    int index = -1;      // parameter table index, for Value, Modulation and NoteModulation
    double value = 0.0;  // host value; a modulation offset in host units; an expression's value
    NoteEvent note{};    // Note
    MidiMessage midi{};  // Midi
    NoteAddress address{};  // Expression and NoteModulation: the notes it applies to; Tuning: port and channel
    NoteExpression::Type expression = NoteExpression::Type::Volume;
    uint32_t tuning = 0;    // Tuning: the tuning the host chose
};

// One host block's audio. Any pointer may be null where the host connected
// nothing: a missing sidechain is silence, a missing aux output is skipped.
template <class T>
struct AudioBuses {
    const T* const* in = nullptr;         // main input; null for an instrument
    T* const* out = nullptr;              // main output; null for a note effect
    const T* const* sidechain = nullptr;  // PRODUCT_SIDECHAIN, [channel]
    T* const* const* aux = nullptr;       // PRODUCT_AUX_OUTPUTS, [bus][channel]
};

// Event sources implement `bool next(EngineEvent&)`.
struct NoEvents {
    bool next(EngineEvent&) { return false; }
};

// Host values, shared by the host's main thread, the editor and the audio
// thread. Writes from anywhere but the audio thread are picked up by the
// engine at the start of its next block or flush.
class ParamStore {
  public:
    ParamStore() { resetToDefaults(); }

    double get(size_t index) const { return values_[index].load(std::memory_order_relaxed); }

    void set(size_t index, double host) {
        values_[index].store(sanitizeHost(params::at(index), host), std::memory_order_relaxed);
        dirty_[index].store(true, std::memory_order_release);
        any_.store(true, std::memory_order_release);
    }

    void resetToDefaults() {
        for (size_t i = 0; i < params::kCount; ++i) set(i, hostDefault(params::at(i)));
    }

    // Engine only.
    void setFromAudio(size_t index, double host) {
        values_[index].store(host, std::memory_order_relaxed);
    }
    template <class Fn>
    void drain(Fn&& fn) {
        if (!any_.exchange(false, std::memory_order_acq_rel)) return;
        for (size_t i = 0; i < params::kCount; ++i)
            if (dirty_[i].exchange(false, std::memory_order_acq_rel)) fn(i);
    }

  private:
    std::atomic<double> values_[params::kCount];
    std::atomic<bool> dirty_[params::kCount];
    std::atomic<bool> any_{false};
};

// Generic over the processor so framework tests can drive probes through
// the real engine; everything else uses the product's, as `Engine`.
template <class P>
class BasicEngine {
  public:
    using Processor = P;
    // How often output parameters are offered to the host.
    static constexpr double kOutputHz = 30.0;

    BasicEngine() {
        for (auto& m : modulation_) m.store(0.0, std::memory_order_relaxed);
        for (size_t i = 0; i < params::kCount; ++i) outputs_[i] = params::at(i).def;
        processor_.bindNoteOutput(&noteOutput_);
        processor_.bindOutputs(outputs_);
        processor_.bindNoteNamesChanged(&namesChanged_);
        if constexpr (properties::kFileCount > 0) files_ = std::make_unique<FileSlots<P>>();
    }

    ParamStore& store() { return store_; }
    const ParamStore& store() const { return store_; }
    // Property values (core/Property.h): set on the main thread, heard by the
    // processor at the start of the next block.
    PropertyStore& properties() { return properties_; }
    const PropertyStore& properties() const { return properties_; }

    // Main thread: sets a property (false when its value is too long). For a
    // file() property, a path that names nothing is first relocated
    // (core/Files.h) -- the property then holds where the file was found --
    // and the file is loaded off the audio thread; `wait` blocks until the
    // processor will have it at its next block, as loading a session or
    // rendering offline wants. Setting what it already holds reloads it,
    // unless it loaded.
    bool setProperty(size_t index, std::string_view value, bool wait = false) {
        if (index >= properties::kCount) return false;
        if (!properties::at(index).isFile() || !files_) return properties_.set(index, value);
        std::string path(value);
        if (!path.empty() && !files::exists(path))
            if (std::string found = files::relocate(path); !found.empty()) path = found;
        const bool same = properties_.get(index) == path;
        if (!properties_.set(index, path)) return false;
        if (!same || files_->status(index).state != FileStatus::State::Ready) files_->request(index, path, wait);
        else if (wait) files_->wait();
        return true;
    }
    // Main thread: the user picked this file property's file. Its folder is
    // remembered for relocating others, and any other file property whose
    // file is missing looks there now.
    void fileChosen(size_t index) {
        if (!files_ || index >= properties::kCount || !properties::at(index).isFile()) return;
        const std::string folder = files::folderOf(properties_.get(index));
        files::rememberFolder(folder);
        for (size_t i = 0; i < properties::kCount; ++i) {
            if (i == index || !properties::at(i).isFile()) continue;
            if (files_->status(i).state != FileStatus::State::Missing) continue;
            if (std::string found = files::relocate(properties_.get(i), {folder}); !found.empty()) setProperty(i, found);
        }
    }
    // What became of a file property's file.
    FileStatus fileStatus(size_t index) const {
        if (!files_ || index >= properties::kCount || !properties::at(index).isFile()) return {};
        return files_->status(index);
    }
    // Main thread: until every file asked for so far is loaded.
    void waitForFiles() {
        if (files_) files_->wait();
    }
    Processor& processor() { return processor_; }
    const Processor& processor() const { return processor_; }

    // --- main thread, while inactive -----------------------------------------

    // Where retuning() comes from, for PRODUCT_TUNING; null for none. It
    // outlives the engine's use of it.
    void setTuning(TuningSource* source) {
        tuning_ = source;
        processor_.bindTuning(source);
    }

    // channelsIn is 0 for an instrument, and both are 0 for a note effect.
    // The sidechain and every aux output are channelsOut wide.
    bool activate(double sampleRate, int maxBlock, int channelsIn, int channelsOut) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlock);
        channelsIn_ = std::max(0, channelsIn);
        channelsOut_ = std::max(0, channelsOut);
        layout_ = channelsOut_ == 1 ? Layout::Mono : channelsOut_ == 2 ? Layout::Stereo : Layout::Generic;
        for (const auto& l : product::info::kLayouts)
            if (l.channels == channelsOut_) layout_ = l.layout;

        const size_t width = static_cast<size_t>(channelsOut_), frames = static_cast<size_t>(maxBlock_);
        scratch_.assign(width, std::vector<double>(frames, 0.0));
        pointers_.resize(width);
        for (size_t c = 0; c < width; ++c) pointers_[c] = scratch_[c].data();
        sidechain_.assign(product::info::kSidechain ? width : 0, std::vector<double>(frames, 0.0));
        sidechainPointers_.resize(sidechain_.size());
        for (size_t c = 0; c < sidechain_.size(); ++c) sidechainPointers_[c] = sidechain_[c].data();
        const size_t auxBuses = static_cast<size_t>(product::info::kAuxOutputs);
        aux_.assign(auxBuses * width, std::vector<double>(frames, 0.0));
        auxChannels_.resize(auxBuses * width);
        auxBuses_.resize(auxBuses);
        for (size_t k = 0; k < aux_.size(); ++k) auxChannels_[k] = aux_[k].data();
        for (size_t b = 0; b < auxBuses; ++b) auxBuses_[b] = auxChannels_.data() + b * width;
        glideBuffer_.assign(params::kSmoothedCount * static_cast<size_t>(maxBlock_), 0.0);
        for (size_t k = 0; k < params::kSmoothedCount; ++k) {
            const ParamDef& p = params::at(params::kSmoothedIndices[k]);
            glides_[k].smoother.setTime(p.smoothing, sampleRate_);
            glides_[k].log = p.taper == Taper::Log;
            // Settled at a billionth of the range (in log space, a billionth
            // of the value): as soon as a large range as a small one.
            glides_[k].smoother.setPrecision(glides_[k].log ? 1e-9 : 1e-9 * std::max(1.0, p.max - p.min));
            glidePointers_[k] = glideBuffer_.data() + k * static_cast<size_t>(maxBlock_);
        }
        editorNotes_.clear();
        for (auto& off : editorOff_) off.store(0, std::memory_order_relaxed);
        for (auto& k : held_) k = 0;
        if constexpr (P::kAudioFeed) {
            if (!feed_) feed_ = std::make_unique<AudioFeed>();
            feed_->clear();
            feedIn_.assign(2, std::vector<float>(frames, 0.0f));
        }
        // Outputs start from their defaults, for the host and the editor.
        for (uint16_t i : params::kOutputIndices) {
            outputs_[i] = params::at(i).def;
            store_.setFromAudio(i, hostDefault(params::at(i)));
        }
        outputPeriod_ = std::max(1, static_cast<int>(std::lround(sampleRate_ / kOutputHz)));
        outputCountdown_ = outputPeriod_;
        outputsDue_ = false;

        // Every value is read fresh, so any pending store changes are moot.
        store_.drain([](size_t) {});
        for (size_t i = 0; i < params::kCount; ++i) {
            mod_[i] = 0.0;
            modulation_[i].store(0.0, std::memory_order_relaxed);
            refresh(i);
        }
        values_.markAll();
        values_.setInitial(true);
        offlineNow_ = offline_.load(std::memory_order_relaxed);
        processor_.prepare(ProcessSetup{sampleRate_, maxBlock_, channelsOut_, layout_, offlineNow_}, values_);
        properties_.deliverAll(
            [this](size_t i, std::string_view value) { processor_.property(properties::at(i).id, value); });
        if (files_) {
            // Loaded for this rate, block size and width before the first block.
            files_->prepare({sampleRate_, maxBlock_, channelsOut_});
            files_->deliverAll([this](size_t i, auto* contents) { processor_.file(properties::at(i).id, contents); });
        }
        retargetGlides(true);
        processor_.update(values_);
        values_.clearChanges();
        voices_.store(processor_.voiceCount(), std::memory_order_relaxed);
        voicesChanged_ = false;

        activeLatency_ = std::max(0, processor_.latencySamples());
        latency_.store(activeLatency_, std::memory_order_relaxed);
        tail_.store(processor_.tailSamples(), std::memory_order_relaxed);
        tailChanged_ = false;
        const int maxLatency = std::max(activeLatency_, processor_.maxLatencySamples());
        bypassDelay_.resize(static_cast<size_t>(channelsOut_));
        for (auto& d : bypassDelay_) d.prepare(maxLatency);
        delayCapacity_ = maxLatency;
        dryLatency_ = activeLatency_;
        bypass_.setTime(10.0, sampleRate_);
        bypass_.set(bypassOn() ? 1.0 : 0.0, true);

        period_ = std::max(1, static_cast<int>(std::lround(sampleRate_ / kTelemetryHz)));
        countdown_ = period_;
        resetPeaks();
        restartWanted_.store(false, std::memory_order_relaxed);
        active_ = true;
        return true;
    }

    void deactivate() { active_ = false; }
    bool active() const { return active_; }

    // Telemetry is produced only while this is on: an editor is open.
    void setMetering(bool on) { metering_.store(on, std::memory_order_relaxed); }
    // Any thread: the host renders without real-time pressure. Heard at the
    // next activation (setup.offline) and the next block (block.offline).
    void setOffline(bool on) { offline_.store(on, std::memory_order_relaxed); }
    bool offline() const { return offline_.load(std::memory_order_relaxed); }
    TelemetryRing& telemetry() { return telemetry_; }
    // The audio for an open editor, for a processor that declared
    // kAudioFeed (after the first activation); null otherwise.
    AudioFeed* feed() { return feed_.get(); }
    // Main thread (the editor's keyboard): a note for the processor, played
    // at the start of the next block on port 0, channel 0, with no note ID.
    // A full queue drops new note-ons; refused note-offs have a per-key mailbox.
    void playNote(int key, double velocity, bool on) {
        if constexpr (product::info::kNoteInput) {
            if (key < 0 || key > 127) return;
            const uint64_t bit = uint64_t{1} << (key & 63);
            if (on && (editorOff_[key >> 6].load(std::memory_order_acquire) & bit)) return;
            if (!editorNotes_.push({static_cast<int16_t>(key), on, static_cast<float>(std::clamp(velocity, 0.0, 1.0))}) && !on)
                editorOff_[key >> 6].fetch_or(bit, std::memory_order_release);
        } else {
            (void)key, (void)velocity, (void)on;
        }
    }

    // Latency reported to the host at the last activation. It may not change
    // until the next one; restartWanted() says when it should.
    int latency() const { return latency_.load(std::memory_order_relaxed); }
    uint32_t tail() const { return tail_.load(std::memory_order_relaxed); }
    double sampleRate() const { return sampleRate_; }
    int channels() const { return channelsOut_; }
    Layout layout() const { return layout_; }
    // The processor's voiceCount() as of its last update.
    int voices() const { return voices_.load(std::memory_order_relaxed); }
    // Audio thread: true once after voiceCount() changed while active.
    bool takeVoiceChange() {
        const bool changed = voicesChanged_;
        voicesChanged_ = false;
        return changed;
    }
    // Set when latency moved, or a restart-flagged parameter changed, while
    // active. The caller asks the host to restart; this clears it.
    bool takeRestartRequest() { return restartWanted_.exchange(false, std::memory_order_acq_rel); }
    // Current modulation offset, for an editor to draw. Host units.
    double modulation(size_t index) const { return modulation_[index].load(std::memory_order_relaxed); }
    uint32_t safetyResets() const { return safetyResets_.load(std::memory_order_relaxed); }
    // Audio thread: true once after tailSamples() changed while active, for
    // the caller to tell the host.
    bool takeTailChange() {
        const bool changed = tailChanged_;
        tailChanged_ = false;
        return changed;
    }
    // Audio thread: true once after the processor called noteNamesChanged().
    bool takeNoteNamesChange() {
        const bool changed = namesChanged_;
        namesChanged_ = false;
        return changed;
    }
    // What the processor sent during the last process() or flush(): note
    // ends, and notes and MIDI of its own.
    const NoteOutput& noteOutput() const { return noteOutput_; }
    // Audio thread: keep unaccepted events for frame zero of the next block.
    void deferNotes(int first) {
        for (int i = first; i < noteOutput_.count(); ++i) {
            const auto& e = noteOutput_[i];
            pendingNotes_.push(e.kind, e.note, e.midi, 0);
        }
        noteOutput_.clear();
    }
    // Audio thread: true about kOutputHz times a second while processing,
    // when the caller should tell the host the output parameters' values
    // (the store's) that changed since it last did.
    bool takeOutputsDue() {
        const bool due = outputsDue_;
        outputsDue_ = false;
        return due;
    }

    // --- audio thread ----------------------------------------------------------

    // Clears every trace of earlier audio, and adopts whatever the store holds
    // now -- a state loaded just before reset() included -- snapped, not
    // glided to: what follows depends only on the current state.
    void reset() SUPPLE_REALTIME {
        if (!active_) return;
        noteOutput_.clear();
        noteOutput_.setOffset(0);
        syncStore();
        processor_.reset();
        for (auto& key : held_) key = 0;
        for (auto& d : bypassDelay_) d.clear();
        bypass_.set(bypassOn() ? 1.0 : 0.0, true);
        reinitialize();
        deferNotes(0);
    }

    // Parameter events with no audio (CLAP params.flush). Valid active or not.
    template <class Source>
    void flush(Source& events) SUPPLE_REALTIME {
        noteOutput_.clear();
        noteOutput_.setOffset(0);
        syncStore();
        EngineEvent e;
        while (events.next(e)) apply(e);
        commit();
    }

    // The main buses only: an effect's input and output, an instrument's
    // output.
    template <class T, class Source>
    void process(const T* const* in, T* const* out, int frames, Source& events,
                 const Transport* transport = nullptr) SUPPLE_REALTIME {
        AudioBuses<T> io;
        io.in = in;
        io.out = out;
        process(io, frames, events, transport);
    }

    template <class T, class Source>
    void process(const AudioBuses<T>& io, int frames, Source& events,
                 const Transport* transport = nullptr) SUPPLE_REALTIME {
        noteOutput_.clear();
        noteOutput_.setOffset(0);
        if (!active_ || frames <= 0 || (!io.out && channelsOut_ > 0)) return;
        ScopedFlushDenormals noDenormals;
        for (int i = 0; i < pendingNotes_.count(); ++i) {
            const auto& e = pendingNotes_[i];
            noteOutput_.push(e.kind, e.note, e.midi, 0);
        }
        pendingNotes_.clear();
        syncStore();
        offlineNow_ = offline_.load(std::memory_order_relaxed);

        const bool metering = metering_.load(std::memory_order_relaxed);
        if (metering != wasMetering_) {
            wasMetering_ = metering;
            countdown_ = period_;
            resetPeaks();
        }
        if constexpr (product::info::kNoteInput) {
            EditorNote n;
            while (editorNotes_.pop(n)) {
                EngineEvent note;
                note.type = EngineEvent::Type::Note;
                note.note.type = n.on ? NoteEvent::Type::On : NoteEvent::Type::Off;
                note.note.key = n.key;
                note.note.velocity = n.velocity;
                noteOutput_.setOffset(0);
                apply(note);
            }
            for (int word = 0; word < 2; ++word) {
                const uint64_t off = editorOff_[word].exchange(0, std::memory_order_acq_rel);
                for (int key = 0; key < 64; ++key) {
                    if (!(off & (uint64_t{1} << key))) continue;
                    EngineEvent note;
                    note.type = EngineEvent::Type::Note;
                    note.note.type = NoteEvent::Type::Off;
                    note.note.key = static_cast<int16_t>(word * 64 + key);
                    note.note.velocity = 0;
                    apply(note);
                }
            }
        }

        EngineEvent e;
        bool pending = events.next(e);
        int frame = 0;
        while (frame < frames) {
            while (pending && static_cast<int>(e.time) <= frame) {
                noteOutput_.setOffset(static_cast<uint32_t>(frame));
                apply(e);
                pending = events.next(e);
            }
            commit();
            int end = std::min(frames, frame + maxBlock_);
            if (pending) end = std::min(end, static_cast<int>(e.time));
            if (metering) end = std::min(end, frame + countdown_);
            run(io, frame, end - frame, metering, transport);
            if (metering) {
                countdown_ -= end - frame;
                if (countdown_ <= 0) {
                    publish();
                    countdown_ = period_;
                }
            }
            frame = end;
        }
        // Events stamped past the end of the block are kept, not dropped.
        noteOutput_.setOffset(static_cast<uint32_t>(frames - 1));
        while (pending) {
            apply(e);
            pending = events.next(e);
        }
        commit();
        publishOutputs(frames);
    }

    // Convenience for tools and tests: one block with no events.
    template <class T>
    void process(const T* const* in, T* const* out, int frames) SUPPLE_REALTIME {
        NoEvents none;
        process(in, out, frames, none, nullptr);
    }

  private:
    static bool flagged(size_t i, uint32_t flag) { return params::at(i).is(flag); }

    bool bypassOn() const {
        return params::kBypassIndex >= 0 && values_.atIndex(static_cast<size_t>(params::kBypassIndex)) > 0.5;
    }

    void refresh(size_t i) {
        const ParamDef& p = params::at(i);
        if (p.isOutput()) return;  // the processor's to report, not to read
        const double host = sanitizeHost(p, store_.get(i) + mod_[i]);
        values_.setIndex(i, toPlain(p, host));
    }

    void syncStore() {
        store_.drain([this](size_t i) { refresh(i); });
        if constexpr (properties::kCount > 0) {
            // While stopped, activate() delivers every value anyway.
            if (active_)
                properties_.deliver(
                    [this](size_t i, std::string_view value) { processor_.property(properties::at(i).id, value); });
        }
        if constexpr (properties::kFileCount > 0) {
            if (active_ && files_) {
                bool delivered = false;
                files_->deliver([this, &delivered](size_t i, auto* contents) {
                    processor_.file(properties::at(i).id, contents);
                    delivered = true;
                });
                if (delivered) reread();
            }
        }
    }

    void apply(const EngineEvent& e) {
        using Type = EngineEvent::Type;
        if (e.type == Type::Tuning) {
            if (tuning_) tuning_->select(e.address.port, e.address.channel, e.tuning);
            return;
        }
        if (e.type == Type::Note || e.type == Type::Midi || e.type == Type::Expression) {
            if (!active_) return;
            commit();  // a note sees every parameter change stamped before it
            if (e.type == Type::Note) {
                hold(e.note);
                // A key the tuning leaves unmapped is not played, and the
                // host hears at once that the note is over.
                if (tuning_ && e.note.type == NoteEvent::Type::On && !tuning_->plays(e.note.key, e.note.channel)) {
                    processor_.noteEnded(e.note);
                    return;
                }
                processor_.note(e.note);
            } else if (e.type == Type::Midi) {
                processor_.midi(e.midi);
            } else if (std::isfinite(e.value)) {
                NoteExpression x;
                x.type = e.expression;
                x.address = e.address;
                x.value = e.value;
                processor_.noteExpression(x);
            }
            return;
        }
        if (e.index < 0 || e.index >= static_cast<int>(params::kCount)) return;
        const size_t i = static_cast<size_t>(e.index);
        const ParamDef& p = params::at(i);
        if (p.isOutput()) return;  // read-only: a host has no business setting it
        if (e.type == Type::NoteModulation) {
            if (!active_ || !p.is(pflag::kPolyMod)) return;
            commit();
            NoteModulation m;
            m.address = e.address;
            m.id = p.id;
            m.amount = std::isfinite(e.value) ? e.value : 0.0;
            processor_.noteModulation(m);
            return;
        }
        if (e.type == EngineEvent::Type::Value) {
            store_.setFromAudio(i, sanitizeHost(p, e.value));
        } else {
            if (!p.is(pflag::kModulatable)) return;
            mod_[i] = std::isfinite(e.value) ? e.value : 0.0;
            modulation_[i].store(mod_[i], std::memory_order_relaxed);
        }
        refresh(i);
    }

    // Which keys are down, for the editor's keyboard.
    void hold(const NoteEvent& n) {
        const bool any = n.key < 0 || n.key > 127;
        if (n.type == NoteEvent::Type::On) {
            if (!any && held_[n.key] < 255) ++held_[n.key];
        } else if (any) {
            for (auto& k : held_) k = 0;
        } else if (n.type == NoteEvent::Type::Choke) {
            held_[n.key] = 0;
        } else if (held_[n.key] > 0) {
            --held_[n.key];
        }
    }

    void commit() {
        if (!values_.anyChanged()) return;
        if (active_) {
            if (params::kAnyRestart && !values_.initial()) {
                for (size_t i = 0; i < params::kCount; ++i)
                    if (flagged(i, pflag::kRestart) && values_.changed(params::at(i).id))
                        restartWanted_.store(true, std::memory_order_release);
            }
            if (params::kBypassIndex >= 0 &&
                values_.changed(params::at(static_cast<size_t>(params::kBypassIndex)).id))
                bypass_.set(bypassOn() ? 1.0 : 0.0);
            for (size_t k = 0; k < params::kSmoothedCount; ++k)
                if (values_.changed(params::at(params::kSmoothedIndices[k]).id)) retarget(k, values_.initial());
            processor_.update(values_);
            reread();
        }
        values_.clearChanges();
    }

    // What the processor reports about itself, after anything that may have
    // changed it: an update, a file.
    void reread() {
        const int voices = processor_.voiceCount();
        if (voices != voices_.load(std::memory_order_relaxed)) {
            voices_.store(voices, std::memory_order_relaxed);
            voicesChanged_ = true;
        }
        const int latency = std::max(0, processor_.latencySamples());
        dryLatency_ = std::min(latency, delayCapacity_);
        if (latency != activeLatency_) restartWanted_.store(true, std::memory_order_release);
        const uint32_t tail = processor_.tailSamples();
        if (tail != tail_.load(std::memory_order_relaxed)) {
            tail_.store(tail, std::memory_order_relaxed);
            tailChanged_ = true;
        }
    }

    void reinitialize() {
        values_.markAll();
        values_.setInitial(true);
        retargetGlides(true);
        processor_.update(values_);
        values_.clearChanges();
    }

    // --- smoothing ------------------------------------------------------------

    struct Glide {
        dsp::Smoother smoother;  // in log space for a log taper
        double target = 0.0;     // plain: what a settled glide reads, exactly
        bool log = false;
        int filled = 0;          // leading buffer frames already holding target
    };

    void retarget(size_t k, bool snap) {
        Glide& g = glides_[k];
        g.target = values_.atIndex(params::kSmoothedIndices[k]);
        g.smoother.set(g.log ? std::log(g.target) : g.target, snap);
        g.filled = 0;
    }
    void retargetGlides(bool snap) {
        for (size_t k = 0; k < params::kSmoothedCount; ++k) retarget(k, snap);
    }

    void fillGlides(int n) {
        for (size_t k = 0; k < params::kSmoothedCount; ++k) {
            Glide& g = glides_[k];
            double* out = glidePointers_[k];
            if (g.smoother.settled()) {
                moving_[k] = false;
                if (g.filled < n) {
                    std::fill(out, out + n, g.target);
                    g.filled = n;
                }
                continue;
            }
            moving_[k] = true;
            g.filled = 0;
            for (int i = 0; i < n; ++i) {
                const double v = g.smoother.next();
                out[i] = g.smoother.settled() ? g.target : g.log ? std::exp(v) : v;
            }
        }
    }

    // --- outputs ----------------------------------------------------------------

    void publishOutputs(int frames) {
        if constexpr (params::kOutputCount > 0) {
            for (uint16_t i : params::kOutputIndices) {
                const ParamDef& p = params::at(i);
                const double host = sanitizeHost(p, toHost(p, outputs_[i]));
                if (host != store_.get(i)) store_.setFromAudio(i, host);
            }
            // The remainder carries over, so the rate holds whatever the
            // block size; a block longer than a period counts once.
            outputCountdown_ -= frames;
            if (outputCountdown_ <= 0) {
                outputCountdown_ = std::max(1, outputCountdown_ + outputPeriod_);
                outputsDue_ = true;
            }
        } else {
            (void)frames;
        }
    }

    template <class T>
    void run(const AudioBuses<T>& io, int offset, int n, bool metering, const Transport* transport) {
        const T* const* in = io.in;
        T* const* out = io.out;
        const int inputs = in ? std::min(channelsIn_, channelsOut_) : 0;
        for (int c = 0; c < channelsOut_; ++c) {
            double* s = scratch_[static_cast<size_t>(c)].data();
            if (c < inputs && in[c]) {
                const T* src = in[c] + offset;
                for (int i = 0; i < n; ++i) s[i] = static_cast<double>(src[i]);
                if (metering && c < kMeterChannels) inputPeak_[c] = std::max(inputPeak_[c], peak(s, n));
                // The input as the host gave it, before processing in place.
                if (P::kAudioFeed && metering && c < 2)
                    for (int i = 0; i < n; ++i) feedIn_[static_cast<size_t>(c)][static_cast<size_t>(i)] = static_cast<float>(src[i]);
            } else {
                std::fill(s, s + n, 0.0);
                if (P::kAudioFeed && metering && c < 2)
                    std::fill_n(feedIn_[static_cast<size_t>(c)].data(), n, 0.0f);
            }
        }

        if constexpr (product::info::kSidechain) {
            for (size_t c = 0; c < sidechain_.size(); ++c) {
                double* s = sidechain_[c].data();
                const T* src = io.sidechain ? io.sidechain[c] : nullptr;
                if (src)
                    for (int i = 0; i < n; ++i) s[i] = static_cast<double>(src[offset + i]);
                else
                    std::fill(s, s + n, 0.0);
            }
        }
        for (auto& a : aux_) std::fill(a.begin(), a.begin() + n, 0.0);

        fillGlides(n);
        Block block;
        block.data = pointers_.data();
        block.channels = channelsOut_;
        block.frames = n;
        block.metering = metering;
        block.offline = offlineNow_;
        block.glides = glidePointers_;
        block.moving = moving_;
        block.sidechain = sidechainPointers_.data();
        block.sidechainConnected = io.sidechain != nullptr;
        block.auxOutputs = static_cast<int>(auxBuses_.size());
        block.auxData = auxBuses_.data();
        if (transport) {
            // Where this split starts, not where the host block did.
            transport_ = *transport;
            if (transport_.playing && transport_.hasTempo && transport_.hasBeats)
                transport_.beats += offset / sampleRate_ * transport_.tempo / 60.0;
            block.transport = &transport_;
        }
        noteOutput_.setOffset(static_cast<uint32_t>(offset));
        processor_.process(block);

        // x * 0 is 0 for every finite x and NaN otherwise: one branch per block.
        double probe = 0.0;
        for (int c = 0; c < channelsOut_; ++c) {
            const double* s = scratch_[static_cast<size_t>(c)].data();
            for (int i = 0; i < n; ++i) probe += s[i] * 0.0;
        }
        for (const auto& a : aux_)
            for (int i = 0; i < n; ++i) probe += a[static_cast<size_t>(i)] * 0.0;
        if (!(probe == 0.0)) {
            for (auto& s : scratch_) std::fill(s.begin(), s.begin() + n, 0.0);
            for (auto& a : aux_) std::fill(a.begin(), a.begin() + n, 0.0);
            processor_.reset();
            reinitialize();
            safetyResets_.fetch_add(1, std::memory_order_relaxed);
        }

        writeOutput(io, offset, n, inputs);

        // Again on what the host receives: the dry path carries the host's
        // own input, and a finite double can overflow a float.
        probe = 0.0;
        for (int c = 0; c < channelsOut_; ++c) {
            if (!out[c]) continue;
            const T* d = out[c] + offset;
            for (int i = 0; i < n; ++i) probe += static_cast<double>(d[i]) * 0.0;
        }
        forEachAuxOut(io, [&](T* d) {
            for (int i = 0; i < n; ++i) probe += static_cast<double>(d[offset + i]) * 0.0;
        });
        if (!(probe == 0.0)) {
            for (int c = 0; c < channelsOut_; ++c)
                if (out[c]) std::fill(out[c] + offset, out[c] + offset + n, T(0));
            forEachAuxOut(io, [&](T* d) { std::fill(d + offset, d + offset + n, T(0)); });
            for (auto& d : bypassDelay_) d.clear();
            processor_.reset();
            reinitialize();
            safetyResets_.fetch_add(1, std::memory_order_relaxed);
        }

        if (metering) {
            for (int c = 0; c < std::min(channelsOut_, kMeterChannels); ++c)
                if (out[c]) outputPeak_[c] = std::max(outputPeak_[c], peakOf(out[c] + offset, n));
            if constexpr (P::kAudioFeed) feedOut(out, offset, n, inputs);
        }
    }

    // Input and output to the editor; a mono bus fills both channels.
    template <class T>
    void feedOut(T* const* out, int offset, int n, int inputs) {
        if (!feed_ || channelsOut_ == 0) return;
        const int ins = std::min(inputs, 2);
        const T* o0 = out[0];
        const T* o1 = channelsOut_ > 1 ? out[1] : out[0];
        feed_->produce(static_cast<size_t>(n), [&](FeedFrame& f, size_t k) {
            f.in[0] = ins > 0 ? feedIn_[0][k] : 0.0f;
            f.in[1] = ins > 1 ? feedIn_[1][k] : f.in[0];
            f.out[0] = o0 ? static_cast<float>(o0[offset + static_cast<int>(k)]) : 0.0f;
            f.out[1] = o1 ? static_cast<float>(o1[offset + static_cast<int>(k)]) : 0.0f;
        });
    }

    // Each connected aux output channel the host gave, with its scratch.
    template <class T, class Fn>
    void forEachAuxOut(const AudioBuses<T>& io, Fn&& fn) {
        if (!io.aux) return;
        for (size_t b = 0; b < auxBuses_.size(); ++b) {
            if (!io.aux[b]) continue;
            for (int c = 0; c < channelsOut_; ++c)
                if (T* d = io.aux[b][c]) fn(d);
        }
    }

    template <class T>
    void writeOutput(const AudioBuses<T>& io, int offset, int n, int inputs) {
        const T* const* in = io.in;
        T* const* out = io.out;
        const size_t width = static_cast<size_t>(channelsOut_);
        const bool hasBypass = params::kBypassIndex >= 0;
        // The common case: not bypassed, nothing to delay. Straight copy.
        if (!hasBypass || (bypass_.settled() && bypass_.value() == 0.0 && dryLatency_ == 0)) {
            for (int c = 0; c < channelsOut_; ++c) {
                if (!out[c]) continue;
                const double* s = scratch_[static_cast<size_t>(c)].data();
                T* dst = out[c] + offset;
                for (int i = 0; i < n; ++i) dst[i] = static_cast<T>(s[i]);
            }
            for (size_t b = 0; io.aux && b < auxBuses_.size(); ++b)
                for (size_t c = 0; io.aux[b] && c < width; ++c)
                    if (T* d = io.aux[b][c]) {
                        const double* s = aux_[b * width + c].data();
                        for (int i = 0; i < n; ++i) d[offset + i] = static_cast<T>(s[i]);
                    }
            return;
        }
        // The dry delay runs whenever latency is non-zero, bypassed or not,
        // so engaging bypass finds it already holding the right history. Aux
        // outputs have no dry signal: bypass fades them to silence.
        for (int i = 0; i < n; ++i) {
            const double b = bypass_.next();
            for (int c = 0; c < channelsOut_; ++c) {
                double dry = (c < inputs && in[c]) ? static_cast<double>(in[c][offset + i]) : 0.0;
                if (dryLatency_ > 0) dry = bypassDelay_[static_cast<size_t>(c)].process(dry, dryLatency_);
                const double wet = scratch_[static_cast<size_t>(c)][static_cast<size_t>(i)];
                const double y = b == 0.0 ? wet : (b == 1.0 ? dry : wet + (dry - wet) * b);
                if (out[c]) out[c][offset + i] = static_cast<T>(y);
            }
            for (size_t k = 0; io.aux && k < auxBuses_.size(); ++k)
                for (size_t c = 0; io.aux[k] && c < width; ++c)
                    if (T* d = io.aux[k][c]) d[offset + i] = static_cast<T>(aux_[k * width + c][static_cast<size_t>(i)] * (1.0 - b));
        }
    }

    static double peak(const double* s, int n) {
        double p = 0.0;
        for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(s[i]));
        return p;
    }
    template <class T>
    static double peakOf(const T* s, int n) {
        double p = 0.0;
        for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(static_cast<double>(s[i])));
        return p;
    }

    void resetPeaks() {
        for (int c = 0; c < kMeterChannels; ++c) inputPeak_[c] = outputPeak_[c] = 0.0;
    }

    void publish() {
        TelemetryFrame f;
        f.channels = std::min(channelsOut_, kMeterChannels);
        for (int c = 0; c < kMeterChannels; ++c) {
            f.inputPeak[c] = static_cast<float>(inputPeak_[c]);
            f.outputPeak[c] = static_cast<float>(outputPeak_[c]);
        }
        for (int k = 0; k < 128; ++k)
            if (held_[k]) f.keys[k >> 6] |= uint64_t{1} << (k & 63);
        processor_.captureTelemetry(f.product);
        telemetry_.push(f);  // full: the editor stalled; this frame is dropped
        resetPeaks();
    }

    Processor processor_;
    ParamStore store_;
    PropertyStore properties_;
    ParamValues values_;
    double mod_[params::kCount]{};
    std::atomic<double> modulation_[params::kCount];
    double outputs_[params::kCount];  // plain, written by the processor
    int outputPeriod_ = 1600;
    int outputCountdown_ = 1600;
    bool outputsDue_ = false;

    static constexpr size_t kGlideSlots = params::kSmoothedCount > 0 ? params::kSmoothedCount : 1;
    Glide glides_[kGlideSlots];
    double* glidePointers_[kGlideSlots]{};
    bool moving_[kGlideSlots]{};
    std::vector<double> glideBuffer_;

    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    int channelsIn_ = 2;
    int channelsOut_ = 2;
    Layout layout_ = Layout::Stereo;
    bool active_ = false;
    std::vector<std::vector<double>> scratch_;
    std::vector<double*> pointers_;
    std::vector<std::vector<double>> sidechain_;  // [channel]
    std::vector<const double*> sidechainPointers_;
    std::vector<std::vector<double>> aux_;  // [bus * width + channel]
    std::vector<double*> auxChannels_;
    std::vector<double* const*> auxBuses_;
    std::atomic<int> voices_{0};
    bool voicesChanged_ = false;

    int activeLatency_ = 0;
    int dryLatency_ = 0;
    int delayCapacity_ = 0;
    std::atomic<int> latency_{0};
    std::atomic<uint32_t> tail_{0};
    bool tailChanged_ = false;
    Transport transport_;
    NoteOutput noteOutput_;
    NoteOutput pendingNotes_;
    std::atomic<bool> restartWanted_{false};
    std::atomic<uint32_t> safetyResets_{0};
    std::vector<dsp::DelayLine> bypassDelay_;
    dsp::Ramp bypass_;

    std::atomic<bool> metering_{false};
    bool wasMetering_ = false;
    std::atomic<bool> offline_{false};
    bool offlineNow_ = false;  // audio thread: offline_ as this block began
    bool namesChanged_ = false;
    TuningSource* tuning_ = nullptr;
    int period_ = 400;
    int countdown_ = 400;
    double inputPeak_[kMeterChannels]{};
    double outputPeak_[kMeterChannels]{};
    TelemetryRing telemetry_;
    struct EditorNote {
        int16_t key = 60;
        bool on = true;
        float velocity = 1.0f;
    };
    Ring<EditorNote, 6> editorNotes_;
    std::atomic<uint64_t> editorOff_[2]{};
    uint8_t held_[128]{};
    std::unique_ptr<AudioFeed> feed_;
    std::vector<std::vector<float>> feedIn_;  // [channel][frame]: this split's input
    // Last: its worker stops before anything a load might touch goes.
    std::unique_ptr<FileSlots<P>> files_;
};

using Engine = BasicEngine<product::Processor>;

}  // namespace supple
