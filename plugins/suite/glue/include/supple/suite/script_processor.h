// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The DSP of a plugin made from a script (script_definition.h), run through
// the framework's engine: the player's, and a source export's.
//
//   - update(): each Param member gets its parameter's plain value (what the
//     script reads), marked changed when it moved, and the script's update()
//     runs. process(): a smoothed() parameter's per-frame values come from
//     the framework's glide (Block::smoothed), then the script's process().
//     Meters (meter()) go to the host through setOutput().
//   - reset() returns every member to what prepare() left: a copy taken
//     after prepare(), copied back without allocating.
//   - Each hook runs with a budget of loop iterations (code the player
//     compiled counts them; C++ does not). A script that spends it is
//     stopped where it was, and stays silent until its next prepare or
//     replacement; its status says where (Instance::status).
//   - A new compile of the same script, with the same parameters, replaces
//     the running one without the host noticing (swap()): prepared on the
//     main thread, taken at the start of a block, crossfaded over 10 ms. The
//     one it replaced goes back to the main thread to be freed (collect()).
//   - Files (AudioFile, Convolver members) are file properties: the
//     framework loads each off the audio thread with loadFile() -- the
//     audio, and an impulse response's convolution too -- and hands it to
//     file(), which points the member at it. A replacement takes the files
//     over, and the code it replaces lets go of them (its convolutions go
//     silent for the crossfade), so a reverb's tail carries on.
//   - An instrument's note expressions (CLAP's: volume, pan, tuning ...)
//     and per-note modulation of its polyModulated() parameters go to the
//     voices whose notes they address, and start neutral with each note.
//   - An instrument's notes go to the framework's voice manager
//     (dsp/Voices.h), which calls the script's voice hooks -- startVoice,
//     releaseVoice, stopVoice -- and which Instrument's methods read and
//     drive (detail::VoiceBank). Its output arrives silent. A replacement
//     picks up the voices sounding: each starts again in the new code, and
//     a released one is released again.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/Notes.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "core/Ring.h"
#include "dsp/Voices.h"
#include "supple/script_files.h"
#include "supple/script_runtime.h"
#include "supple/suite/script_definition.h"

namespace supple::suite {

namespace S = supple::script;
namespace D = supple::script::detail;

// One build of a script's running struct, its post-prepare copy, its Param
// members by parameter table index, and its file members by property index.
class Generation {
  public:
    Generation(std::shared_ptr<const D::Code> code, const ParamTable& table,
               const PropertyList& properties, D::Instance* instance)
        : program_(std::move(code)), self_(program_->create()) {
        D::Instance::bind(*static_cast<S::Plugin*>(self_), instance);
        params_.assign(table.count(), nullptr);
        for (const D::ParamModel& m : program_->params()) {
            const int i = table.indexOf(m.id);
            if (i < 0) continue;
            auto* p = reinterpret_cast<S::Param*>(static_cast<unsigned char*>(self_) + m.offset);
            params_[static_cast<size_t>(i)] = p;
            D::ParamBinding::setSlot(*p, i);  // forVoice() finds its modulation by it
            const ParamDef& def = table.at(static_cast<size_t>(i));
            D::ParamBinding::setRange(*p, def.min, def.is(pflag::kOffAtMin));
        }
        files_.assign(properties.count(), -1);
        for (const D::FileModel& m : program_->files())
            if (const int i = properties.indexOf(m.id); i >= 0)
                files_[static_cast<size_t>(i)] = m.offset;
    }
    ~Generation() {
        if (reset_) program_->destroy(reset_);
        program_->destroy(self_);
    }
    Generation(const Generation&) = delete;
    Generation& operator=(const Generation&) = delete;

    const D::Code& program() const { return *program_; }
    void* self() const { return self_; }
    S::Param* param(size_t index) const { return params_[index]; }
    size_t paramCount() const { return params_.size(); }
    // Points file property `index`'s member at `contents` (null: none).
    void bindFile(size_t index, D::FileContents* contents) {
        if (index < files_.size() && files_[index] >= 0)
            D::FileBinding::bind(static_cast<unsigned char*>(self_) + files_[index], contents);
    }

    // Main thread: the script's prepare(), then the copy reset() returns to.
    void prepare(D::Instance* instance) {
        D::CurrentScope scope(instance);
        instance->budget = std::int64_t{1} << 40;
        if (auto hook = program_->prepare()) hook(instance, self_);
        if (!reset_) reset_ = program_->create();
        program_->copy(reset_, self_);
    }
    // Audio thread: back to what prepare() left.
    void reset() {
        if (reset_) program_->copy(self_, reset_);
    }
    // The runtime record its code runs with, in the struct and its copy.
    void bindInstance(D::Instance* instance) {
        D::Instance::bind(*static_cast<S::Plugin*>(self_), instance);
        if (reset_) D::Instance::bind(*static_cast<S::Plugin*>(reset_), instance);
    }

  private:
    std::shared_ptr<const D::Code> program_;
    void* self_;
    void* reset_ = nullptr;
    std::vector<S::Param*> params_;
    std::vector<int> files_;  // each file property's member offset, or -1
};

class ScriptProcessor : public ProcessorBase<ScriptProcessor> {
  public:
    explicit ScriptProcessor(const DefinitionPtr& definition)
        : definition_(std::static_pointer_cast<const ScriptDefinition>(definition)),
          table_(&definition_->params()),
          current_(std::make_unique<Generation>(definition_->code(), *table_,
                                                definition_->properties(), &instance_)),
          files_(definition_->properties().count(), nullptr) {
        const D::InfoModel& info = definition_->code()->info();
        latency_ = std::max(0, info.latency);
        tail_ = info.tail < 0 ? UINT32_MAX : static_cast<uint32_t>(info.tail);
        instrument_ = definition_->code()->kind() == D::PluginKind::Instrument;
        voiceCount_ = std::clamp(info.voices > 0 ? info.voices : kDefaultVoices, 1, kMaxVoices);
        if (instrument_) instance_.voices = &bank_;
        polySlot_.assign(table_->count(), -1);
        for (size_t i = 0; i < table_->count(); ++i)
            if (instrument_ && table_->at(i).is(pflag::kPolyMod)) {
                polySlot_[i] = static_cast<int>(polyAmount_.size());
                polyAmount_.emplace_back();
            }
        for (int v = 0; v < kMaxVoices; ++v) noteBegins(v);
    }
    ~ScriptProcessor() {
        delete incoming_.exchange(nullptr, std::memory_order_acq_rel);
        collect();
    }

    // --- the framework's ---------------------------------------------------------

    void prepare(const ProcessSetup& setup, const ParamValues& v) {
        instance_.sampleRate = setup.sampleRate;
        instance_.maxBlockSize = setup.maxBlockSize;
        instance_.channels = setup.channels;
        instance_.offline = setup.offline;
        // A replacement offered before this prepare is prepared again for it.
        if (Generation* waiting = incoming_.exchange(nullptr, std::memory_order_acq_rel))
            current_.reset(waiting);
        fading_.reset();
        if (instrument_) {
            voices_.prepare(setup.sampleRate);
            voices_.setCount(voiceCount_, *this);
        }
        load(*current_, v, true);
        bindFiles(*current_);
        current_->prepare(&instance_);
        stopped_ = false;
        const size_t channels = static_cast<size_t>(std::max(setup.channels, 0));
        const size_t frames = static_cast<size_t>(std::max(setup.maxBlockSize, 1));
        fadeBuffer_.assign(channels, std::vector<double>(frames, 0.0));
        fadePointers_.resize(channels);
        for (size_t c = 0; c < channels; ++c) fadePointers_[c] = fadeBuffer_[c].data();
        fadeLength_ = std::max(1, static_cast<int>(setup.sampleRate * 0.01));
    }

    void update(const ParamValues& v) {
        if (fading_) run(*fading_, v, false);
        run(*current_, v, false);
        applyVoiceMode();
    }

    void reset() {
        if (instrument_) voices_.reset(*this);
        current_->reset();  // which restores the files prepare() saw
        bindFiles(*current_);
        for (D::FileContents* f : files_)
            if (f) f->reset();
        if (fading_) retire(std::move(fading_));
        stopped_ = false;
    }

    // Files: what loadFile() made of a file property's file, at the start of
    // a block (null: none, a missing one, one that failed).
    using FileContents = D::FileContents;
    void file(uint32_t id, D::FileContents* contents) {
        const int i = definition_->properties().indexOf(id);
        if (i < 0) return;
        files_[static_cast<size_t>(i)] = contents;
        current_->bindFile(static_cast<size_t>(i), contents);
        if (fading_) fading_->bindFile(static_cast<size_t>(i), nullptr);  // may be freed soon
    }
    // On the framework's loading thread: the file's audio, resampled to the
    // session's rate (an impulse response's first 20 seconds, a sample's
    // first 10 minutes), and an impulse response's convolution, prepared for
    // the session's channels.
    static std::unique_ptr<D::FileContents> loadFile(const FileRequest& r, std::string& error) {
        const bool impulse = r.kind == static_cast<uint32_t>(D::FileModel::Kind::Impulse);
        auto audio = ProcessorBase<ScriptProcessor>::loadFile(r, error, impulse ? 20.0 : 600.0);
        if (!audio) return nullptr;
        if (audio->frames() == 0 || audio->channels() == 0) {
            error = "the file holds no audio";
            return nullptr;
        }
        auto contents = std::make_unique<D::FileContents>();
        contents->audio = std::move(*audio);
        if (impulse) {
            const dsp::AudioData& ir = contents->audio;
            contents->length = static_cast<int>(ir.frames());
            contents->convolution = std::make_unique<dsp::Convolution>();
            contents->convolution->prepare(ir.data().data(), ir.channels(), ir.frames(),
                                           std::max(1, r.channels));
        }
        return contents;
    }
    static std::string describeFile(const D::FileContents& c) {
        char line[64];
        std::snprintf(line, sizeof(line), "%d ch, %.2f s", c.audio.channels(), c.audio.seconds());
        return line;
    }

    // An instrument's notes and controllers (pitch bend, the sustain pedal).
    void note(const NoteEvent& e) {
        if (instrument_) voices_.note(e, *this);
    }
    void midi(const MidiMessage& m) {
        if (instrument_) voices_.midi(m, *this);
    }
    // To the voices whose notes they address.
    void noteExpression(const NoteExpression& x) {
        if (!instrument_) return;
        for (int v = 0; v < voiceCount_; ++v)
            if (voices_[v].active && x.address.matches(voices_[v].note))
                expression_[static_cast<size_t>(v)][static_cast<size_t>(x.type)] = x.value;
    }
    void noteModulation(const NoteModulation& m) {
        const int i = table_->indexOf(m.id);
        if (i < 0 || polySlot_[static_cast<size_t>(i)] < 0) return;
        auto& amounts = polyAmount_[static_cast<size_t>(polySlot_[static_cast<size_t>(i)])];
        for (int v = 0; v < voiceCount_; ++v)
            if (voices_[v].active && m.address.matches(voices_[v].note))
                amounts[static_cast<size_t>(v)] = m.amount;
    }

    void process(Block& block) {
        if (Generation* next = incoming_.exchange(nullptr, std::memory_order_acq_rel)) adopt(next);
        if (stopped_) {
            silence(block);
            return;
        }
        if (instrument_) silence(block);  // the voices add into it
        endFrame_ = std::max(block.frames - 1, 0);
        if (fading_) {
            // The old generation plays on a copy of the input; the two meet
            // along a linear fade.
            for (size_t c = 0; c < fadePointers_.size() && static_cast<int>(c) < block.channels;
                 ++c)
                std::copy(block.data[c], block.data[c] + block.frames, fadePointers_[c]);
            Block copy = block;
            copy.data = fadePointers_.data();
            fadingNow_ = true;  // its voices end with it, not in the voice manager
            runProcess(*fading_, copy);
            fadingNow_ = false;
            runProcess(*current_, block);
            for (int i = 0; i < block.frames; ++i) {
                const double t = std::min(1.0, static_cast<double>(fadeDone_ + i) / fadeLength_);
                for (int c = 0; c < block.channels && c < static_cast<int>(fadePointers_.size());
                     ++c)
                    block.data[c][i] =
                        fadePointers_[static_cast<size_t>(c)][i] * (1.0 - t) + block.data[c][i] * t;
            }
            fadeDone_ += block.frames;
            if (fadeDone_ >= fadeLength_) retire(std::move(fading_));
        } else {
            runProcess(*current_, block);
        }
        if (stopped_) silence(block);
        report(*current_);
        if (instrument_) voices_.advance(block.frames);
        endFrame_ = 0;
    }

    // setLatency()'s, from prepare(), or Info's.
    int latencySamples() const { return instance_.latency >= 0 ? instance_.latency : latency_; }
    uint32_t tailSamples() const { return tail_; }

    // --- the layer's (main thread) ---------------------------------------------------

    // The script's runtime record: its sample rate and block size, and its status.
    const D::Instance& instance() const { return instance_; }

    // Offers a new build of the script, with the same parameters, to
    // replace the running one at the start of a block. Main thread, after
    // the first prepare.
    void swap(std::shared_ptr<const D::Code> code, const ParamValues& values) {
        auto next = std::make_unique<Generation>(std::move(code), *table_,
                                                 definition_->properties(), &instance_);
        load(*next, values, true);
        // Prepared here while the audio thread runs the current code, so
        // against a record of its own -- the same rate and sizes, no voices
        // -- that its hooks' budget and setLatency() write; the real one
        // from the swap on.
        D::Instance own;
        own.sampleRate = instance_.sampleRate;
        own.maxBlockSize = instance_.maxBlockSize;
        own.channels = instance_.channels;
        own.offline = instance_.offline;
        next->bindInstance(&own);
        next->prepare(&own);
        next->bindInstance(&instance_);
        delete incoming_.exchange(next.release(), std::memory_order_acq_rel);
    }

    // Frees what the audio thread let go of.
    void collect() {
        Generation* done = nullptr;
        while (retired_.pop(done)) delete done;
    }

    // --- the voice manager's (dsp/Voices.h): the script's voice hooks -------------

    void voiceStart(int v, bool retrigger) {
        noteBegins(v);
        startVoice(v, retrigger);
    }
    void startVoice(int v, bool retrigger) {
        const D::Code& code = current_->program();
        if (auto hook = code.startVoiceRetrigger())
            callVoice([&] { hook(&instance_, current_->self(), v, retrigger ? 1 : 0); });
        else if (auto start = code.startVoice(); start && retrigger)
            callVoice([&] { start(&instance_, current_->self(), v); });
    }
    void voiceRelease(int v) {
        if (auto hook = current_->program().releaseVoice())
            callVoice([&] { hook(&instance_, current_->self(), v); });
    }
    void voiceStop(int v) {
        if (auto hook = current_->program().stopVoice())
            callVoice([&] { hook(&instance_, current_->self(), v); });
    }
    void voiceEnded(const NoteEvent& n) { noteEnded(n, endFrame_); }

  private:
    // Every value into a generation's Param members.
    void load(Generation& g, const ParamValues& v, bool all) {
        for (size_t i = 0; i < g.paramCount(); ++i) {
            S::Param* p = g.param(i);
            if (!p || table_->at(i).isOutput()) continue;
            const uint32_t id = table_->at(i).id;
            D::ParamBinding::setValue(*p, v[id], all || v.initial() || v.changed(id));
        }
    }

    void run(Generation& g, const ParamValues& v, bool all) {
        load(g, v, all);
        D::CurrentScope scope(&instance_);
        instance_.budget = kBudgetPerHook;
        if (auto hook = g.program().update()) hook(&instance_, g.self());
        checkBudget();
        for (size_t i = 0; i < g.paramCount(); ++i)
            if (S::Param* p = g.param(i)) D::ParamBinding::setValue(*p, p->value(), false);
    }

    void runProcess(Generation& g, Block& block) {
        S::Block io;
        io.channels = block.channels;
        io.frames = block.frames;
        io.data_ = block.data;
        io.sidechainConnected = block.sidechainConnected;
        io.sidechain_ = block.sidechain;
        io.sidechainChannels_ = block.sidechain ? block.channels : 0;
        io.aux_ = block.auxData;
        io.outputs_ = block.auxOutputs;
        spare_ = 0.0;
        io.spare_ = &spare_;
        if (block.transport) {
            io.transport.playing = block.transport->playing;
            io.transport.tempo = block.tempo();
            io.transport.beats = block.transport->beats;
            io.transport.barStart = block.transport->barStart;
            io.transport.timeSigNumerator = block.transport->timeSigNumerator;
            io.transport.timeSigDenominator = block.transport->timeSigDenominator;
        }
        for (uint16_t i : table_->smoothed())
            if (S::Param* p = g.param(i))
                D::ParamBinding::setGlide(*p, block.smoothed(table_->at(i).id), block.frames);
        {
            D::CurrentScope scope(&instance_);
            instance_.budget = kBudgetPerFrame * std::max(block.frames, 1);
            if (auto hook = g.program().process()) hook(&instance_, g.self(), &io);
        }
        for (uint16_t i : table_->smoothed())
            if (S::Param* p = g.param(i)) D::ParamBinding::setGlide(*p, nullptr, 0);
        checkBudget();
    }

    void adopt(Generation* next) {
        // Its values are the running one's; it starts from them, snapped.
        for (size_t i = 0; i < next->paramCount() && i < current_->paramCount(); ++i)
            if (S::Param* p = next->param(i); p && current_->param(i))
                D::ParamBinding::setValue(*p, current_->param(i)->value(), true);
        {
            D::CurrentScope scope(&instance_);
            instance_.budget = kBudgetPerHook;
            if (auto hook = next->program().update()) hook(&instance_, next->self());
        }
        if (fading_) retire(std::move(fading_));
        fading_ = std::move(current_);
        current_.reset(next);
        // The files are the new code's now; the old lets go of them.
        bindFiles(*current_);
        for (size_t i = 0; i < files_.size(); ++i) fading_->bindFile(i, nullptr);
        fadeDone_ = 0;
        stopped_ = false;
        // The voices sounding carry on in the new code: each starts again,
        // and a released one is released again.
        if (instrument_)
            for (int v = 0; v < voiceCount_; ++v)
                if (voices_[v].active) {
                    startVoice(v, true);  // the same note: its expressions stay
                    if (!voices_[v].held) voiceRelease(v);
                }
    }

    // A new note on voice v: its expressions neutral, its modulation none.
    void noteBegins(int v) {
        expression_[static_cast<size_t>(v)] = {1.0, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0};
        for (auto& amounts : polyAmount_) amounts[static_cast<size_t>(v)] = 0.0;
    }

    void bindFiles(Generation& g) {
        for (size_t i = 0; i < files_.size(); ++i) g.bindFile(i, files_[i]);
    }

    // A voice hook: the script runs inside its instance, on a budget.
    template <class Call>
    void callVoice(Call&& call) {
        if (stopped_) return;
        {
            D::CurrentScope scope(&instance_);
            instance_.budget = kBudgetPerHook;
            call();
        }
        checkBudget();
    }

    // A mode set in update() takes effect once it returns: the change
    // releases voices, through the script's hooks.
    void applyVoiceMode() {
        if (pendingMode_ < 0) return;
        const auto mode = static_cast<Voices::Mode>(pendingMode_);
        pendingMode_ = -1;
        voices_.setMode(mode, *this);
    }

    // Instrument's methods, through the instance (script_runtime.h).
    class Bank final : public D::VoiceBank {
      public:
        explicit Bank(ScriptProcessor& p) : p_(p) {}
        int count() const override { return p_.voiceCount_; }
        bool playing(int v) const override { return p_.voices_[v].active; }
        int key(int v) const override { return p_.voices_[v].note.key; }
        int channel(int v) const override { return p_.voices_[v].note.channel; }
        double velocity(int v) const override { return p_.voices_[v].velocity; }
        double pitch(int v, int frame) const override {
            return p_.voices_.pitchAt(v, frame) + p_.expression_[static_cast<size_t>(v)][2];
        }
        double expression(int v, int which) const override {
            return which >= 0 && which < 7
                       ? p_.expression_[static_cast<size_t>(v)][static_cast<size_t>(which)]
                       : 0.0;
        }
        double modulated(int slot, int v, double value) const override {
            if (slot < 0 || static_cast<size_t>(slot) >= p_.polySlot_.size()) return value;
            const int k = p_.polySlot_[static_cast<size_t>(slot)];
            if (k < 0) return value;
            return modulatedPlain(p_.table_->at(static_cast<size_t>(slot)), value,
                                  p_.polyAmount_[static_cast<size_t>(k)][static_cast<size_t>(v)]);
        }
        void finished(int v) override {
            if (!p_.fadingNow_) p_.voices_.finished(v, p_);
        }
        void setMode(S::VoiceMode mode) override { p_.pendingMode_ = static_cast<int>(mode); }
        void setGlide(double ms) override { p_.voices_.setGlide(ms); }
        void setBendRange(double semitones) override { p_.voices_.setBendRange(semitones); }

      private:
        ScriptProcessor& p_;
    };

    void retire(std::unique_ptr<Generation> g) {
        // A full queue keeps it: freeing here would be the audio thread's to do.
        Generation* raw = g.release();
        if (!retired_.push(raw)) fading_.reset(raw);
    }

    void checkBudget() {
        const uint32_t exceeded = instance_.status.budgetExceeded.load(std::memory_order_relaxed);
        if (exceeded != budgetSeen_) {
            budgetSeen_ = exceeded;
            stopped_ = true;
        }
    }

    static void silence(Block& block) {
        for (int c = 0; c < block.channels; ++c)
            std::fill(block.data[c], block.data[c] + block.frames, 0.0);
        for (int b = 0; b < block.auxOutputs; ++b)
            for (int c = 0; c < block.channels; ++c)
                std::fill(block.aux(b)[c], block.aux(b)[c] + block.frames, 0.0);
    }

    void report(const Generation& g) {
        for (uint16_t i : table_->outputs())
            if (const S::Param* p = g.param(i))
                setOutput(table_->at(i).id, D::ParamBinding::reported(*p));
    }

    // Loop iterations a hook may take: generous for real work, small enough
    // that a runaway loop is stopped within a block or so.
    static constexpr int kMaxVoices = 64, kDefaultVoices = 8;
    using Voices = dsp::Voices<kMaxVoices>;
    static_assert(static_cast<int>(S::VoiceMode::Legato) == static_cast<int>(Voices::Mode::Legato));

    static constexpr std::int64_t kBudgetPerHook = std::int64_t{1} << 26;
    static constexpr std::int64_t kBudgetPerFrame = std::int64_t{1} << 16;

    std::shared_ptr<const ScriptDefinition> definition_;
    const ParamTable* table_;
    D::Instance instance_;
    std::unique_ptr<Generation> current_;
    std::unique_ptr<Generation> fading_;
    std::vector<D::FileContents*> files_;  // by property index: the framework's, lent
    std::atomic<Generation*> incoming_{nullptr};
    Ring<Generation*, 3> retired_;
    std::vector<std::vector<double>> fadeBuffer_;
    std::vector<double*> fadePointers_;
    int fadeLength_ = 480, fadeDone_ = 0;
    double spare_ = 0.0;
    bool stopped_ = false;
    uint32_t budgetSeen_ = 0;
    int latency_ = 0;
    uint32_t tail_ = 0;
    // An instrument's.
    bool instrument_ = false;
    int voiceCount_ = kDefaultVoices;
    Voices voices_;
    Bank bank_{*this};
    int pendingMode_ = -1;
    int endFrame_ = 0;
    // Each voice's note expressions (CLAP's order), and per poly-modulated
    // parameter (polySlot_, by table index) each voice's modulation.
    std::array<std::array<double, 7>, kMaxVoices> expression_{};
    std::vector<int> polySlot_;
    std::vector<std::array<double, kMaxVoices>> polyAmount_;
    bool fadingNow_ = false;
};

}  // namespace supple::suite
