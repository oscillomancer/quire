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
//     main thread (refused if its prepare() reports another latency), taken
//     at the start of a block, crossfaded over 10 ms. The
//     one it replaced goes back to the main thread to be freed (collect()).
//     What the old code observes while it fades out (an index out of range,
//     its budget spent) is set aside, not the new code's; old code its
//     budget stopped is silent, so the new code fades in from silence.
//   - Files (AudioFile, Convolver members) are file properties: the
//     framework loads each off the audio thread with loadFile() -- the
//     audio, and an impulse response's convolution too -- and hands it to
//     file(), which points the member at it. A replacement takes the files
//     over, and the code it replaces lets go of them (its convolutions go
//     silent for the crossfade), so a reverb's tail carries on.
//   - An instrument's note expressions (CLAP's: volume, pan, tuning ...)
//     and per-note modulation of its polyModulated() parameters go to the
//     voices whose notes they address, and start neutral with each note.
//   - Shared and SharedArray members go to the script's editor whenever
//     telemetry is due (while an editor is open or the layer watches),
//     through the generation's SharedChannel (script_shared.h).
//     editorCode() and editorChannel() are what an editor opened now shows:
//     the newest code offered, and its channel.
//   - An instrument's notes go to the framework's voice manager
//     (dsp/Voices.h), which calls the script's voice hooks -- startVoice,
//     releaseVoice, stopVoice -- and which Instrument's methods read and
//     drive (detail::VoiceBank). Its output arrives silent. A replacement
//     picks up the voices sounding: each starts again in the new code, and
//     a released one is released again. setVoiceLimit() plays fewer of the
//     voices, which CLAP's voice-info reports.
//   - Routes (script_definition.h's): the routes property, read into a
//     fixed table on the audio thread as it arrives, without allocating.
//     Param::routed() and forVoice() ask it (detail::RouteHost) for a
//     parameter's routes, from the values the sources have then: the
//     script's Source members, in the generation whose hook is running, and
//     an instrument's own -- its notes' velocity, key, pressure and slide, a
//     random value drawn for each note from a sequence that starts again at
//     every reset, and the controllers last received on any channel.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/Notes.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "core/Ring.h"
#include "dsp/Voices.h"
#include "supple/script_files.h"
#include "supple/script_runtime.h"
#include "supple/suite/script_definition.h"
#include "supple/suite/script_shared.h"

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
        if (!program_->shared().empty())
            channel_ = std::make_shared<SharedChannel>(program_->shared());
    }
    ~Generation() {
        if (reset_) program_->destroy(reset_);
        program_->destroy(self_);
    }
    Generation(const Generation&) = delete;
    Generation& operator=(const Generation&) = delete;

    const D::Code& program() const { return *program_; }
    void* self() const { return self_; }
    // Its Shared members' way to the editor; null when it has none.
    const std::shared_ptr<SharedChannel>& channel() const { return channel_; }
    // Audio thread: the Shared members, as they are, to the editor.
    void publish() const noexcept {
        if (channel_) channel_->publish(self_);
    }
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
        preparedTail = instance->tail;
        if (!reset_) reset_ = program_->create();
        program_->copy(reset_, self_);
    }
    // Audio thread: back to what prepare() left.
    void reset() {
        if (reset_) program_->copy(self_, reset_);
    }
    // What a replacement's prepare() asked of the voices (setVoiceMode,
    // setGlide, setBendRange), for the swap to apply; NaN or -1 when nothing.
    struct VoiceSettings {
        int mode = -1;
        double glideMs = std::numeric_limits<double>::quiet_NaN();
        double bendRange = std::numeric_limits<double>::quiet_NaN();
        int limit = -1;
    };
    VoiceSettings preparedVoices;
    // setTail() writes the runtime record, outside the copied script struct.
    int preparedTail = D::Instance::kInfoTail;

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
    std::shared_ptr<SharedChannel> channel_;
};

class ScriptProcessor : public ProcessorBase<ScriptProcessor> {
  public:
    // Script::offline() reports it.
    static constexpr bool kOfflineMode = true;

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
        setUpRoutes();
        polySlot_.assign(table_->count(), -1);
        for (size_t i = 0; i < table_->count(); ++i)
            if (instrument_ && table_->at(i).is(pflag::kPolyMod)) {
                polySlot_[i] = static_cast<int>(polyAmount_.size());
                polyAmount_.emplace_back();
            }
        for (int v = 0; v < kMaxVoices; ++v) noteBegins(v);
        editorCode_ = definition_->code();
        editorChannel_ = current_->channel();
    }
    ~ScriptProcessor() {
        delete incoming_.exchange(nullptr, std::memory_order_acq_rel);
        collect();
    }

    // --- the framework's ---------------------------------------------------------

    void prepare(const ProcessSetup& setup, const ParamValues& v) {
        instance_.status.clear();
        budgetSeen_ = 0;
        instance_.sampleRate = setup.sampleRate;
        instance_.maxBlockSize = setup.maxBlockSize;
        instance_.channels = setup.channels;
        instance_.offline = setup.offline;
        instance_.tail = D::Instance::kInfoTail;
        offlineNow_.store(setup.offline, std::memory_order_relaxed);
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
        resetControllers();
        running_ = current_.get();
        current_->prepare(&instance_);
        applyVoiceLimit();
        stopped_ = false;
        const size_t channels = static_cast<size_t>(std::max(setup.channels, 0));
        const size_t frames = static_cast<size_t>(std::max(setup.maxBlockSize, 1));
        fadeBuffer_.assign(channels, std::vector<double>(frames, 0.0));
        fadePointers_.resize(channels);
        for (size_t c = 0; c < channels; ++c) fadePointers_[c] = fadeBuffer_[c].data();
        const size_t auxiliaries = definition_->info().auxOutputs.size();
        fadeAuxBuffer_.assign(auxiliaries * channels, std::vector<double>(frames, 0.0));
        fadeAuxPointers_.resize(fadeAuxBuffer_.size());
        for (size_t c = 0; c < fadeAuxPointers_.size(); ++c)
            fadeAuxPointers_[c] = fadeAuxBuffer_[c].data();
        fadeAuxBuses_.resize(auxiliaries);
        for (size_t b = 0; b < auxiliaries; ++b)
            fadeAuxBuses_[b] = fadeAuxPointers_.data() + b * channels;
        fadeLength_ = std::max(1, static_cast<int>(setup.sampleRate * 0.01));
    }

    void update(const ParamValues& v) {
        if (fading_ && !fadingStopped_) runFading([&] { run(*fading_, v, false); });
        run(*current_, v, false);
        checkBudget();
        applyVoiceMode();
        applyVoiceLimit();
    }

    void reset() {
        instance_.status.clear();
        budgetSeen_ = 0;
        if (instrument_) voices_.reset(*this);
        resetControllers();
        current_->reset();  // which restores the files prepare() saw
        instance_.tail = current_->preparedTail;
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

    // An instrument's notes and controllers (pitch bend, the sustain pedal,
    // and those the routes' sources follow).
    void note(const NoteEvent& e) {
        if (instrument_) voices_.note(e, *this);
    }
    void midi(const MidiMessage& m) {
        if (!instrument_) return;
        voices_.midi(m, *this);
        if (m.isPitchBend()) pitchBend_ = m.pitchBend();
        if (m.isChannelPressure()) aftertouch_ = m.pressure();
        if (m.isController() && m.controller() >= 0 && m.controller() < 128)
            controllers_[static_cast<size_t>(m.controller())] = m.value() / 127.0;
    }

    // Properties: the routes, into the table the audio thread reads.
    void property(uint32_t id, std::string_view value) {
        if (id == kRoutesProperty && definition_->meaning().routes >= 0) loadRoutes(value);
    }

    // CLAP voice-info: the voices the mode and the limit let play.
    int voiceCount() const { return instrument_ ? voices_.voiceCount() : 0; }
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
        // A trace counts the frames processed since it began, whatever
        // returns early below.
        struct Advance {
            D::TraceLog* log;
            int frames;
            ~Advance() {
                if (log) log->at += frames;
            }
        } advance{instance_.trace.load(std::memory_order_acquire), block.frames};
        if (block.offline != instance_.offline) {
            instance_.offline = block.offline;
            offlineNow_.store(block.offline, std::memory_order_relaxed);
        }
        // Keep an offered replacement on the main-thread side until the
        // current fade can be retired. Otherwise adopt() would overwrite
        // the generation retire() retained when its queue was full.
        if (!fading_ || retired_.writable())
            if (Generation* next = incoming_.exchange(nullptr, std::memory_order_acq_rel))
                adopt(next);
        if (stopped_) {
            silence(block);
            return;
        }
        if (instrument_) silence(block);  // the voices add into it
        endFrame_ = std::max(block.frames - 1, 0);
        if (fading_) {
            // The old generation plays on a copy of the input; the two meet
            // along a linear fade.
            const size_t channels =
                std::min(fadePointers_.size(), static_cast<size_t>(std::max(block.channels, 0)));
            for (size_t c = 0; c < channels; ++c)
                std::copy(block.data[c], block.data[c] + block.frames, fadePointers_[c]);
            Block copy = block;
            copy.data = fadePointers_.data();
            // Extra outputs are silent on entry for each generation, and
            // need the same fade as the main output. Sharing the host's
            // buffers would let old code's writes enter the new code.
            copy.auxData = fadeAuxBuses_.data();
            for (auto& channel : fadeAuxBuffer_) std::fill_n(channel.data(), block.frames, 0.0);
            if (!fadingStopped_) {
                fadingNow_ = true;  // its voices end with it, not in the voice manager
                runFading([&] { runProcess(*fading_, copy); });
                fadingNow_ = false;
            }
            if (fadingStopped_) silence(copy);
            runProcess(*current_, block);
            for (int i = 0; i < block.frames; ++i) {
                const double t = std::min(1.0, static_cast<double>(fadeDone_ + i) / fadeLength_);
                for (int c = 0; c < block.channels && c < static_cast<int>(fadePointers_.size());
                     ++c)
                    block.data[c][i] =
                        fadePointers_[static_cast<size_t>(c)][i] * (1.0 - t) + block.data[c][i] * t;
                for (int b = 0; b < block.auxOutputs; ++b)
                    for (int c = 0; c < block.channels; ++c)
                        block.aux(b)[c][i] = copy.aux(b)[c][i] * (1.0 - t) + block.aux(b)[c][i] * t;
            }
            fadeDone_ += block.frames;
            if (fadeDone_ >= fadeLength_) retire(std::move(fading_));
        } else {
            runProcess(*current_, block);
        }
        checkBudget();
        if (stopped_) silence(block);
        report(*current_);
        if (instrument_) voices_.advance(block.frames);
        endFrame_ = 0;
    }

    // Whenever telemetry is due: the Shared members to the editor.
    template <class T>
    void captureTelemetry(T&) {
        current_->publish();
    }

    // setLatency()'s, from prepare(), or Info's.
    int latencySamples() const { return instance_.latency >= 0 ? instance_.latency : latency_; }
    // setTail()'s, from any hook but the editor's, or Info's.
    uint32_t tailSamples() const {
        const int set = instance_.tail;
        if (set == D::Instance::kInfoTail) return tail_;
        return set < 0 ? UINT32_MAX : static_cast<uint32_t>(set);
    }

    // --- the layer's (main thread) ---------------------------------------------------

    // The script's runtime record: its sample rate and block size, and its status.
    const D::Instance& instance() const { return instance_; }
    // Where trace() records from the next block on (null: nowhere). Main
    // thread, while the plugin is inactive; `log` outlives its use here.
    void trace(D::TraceLog* log) { instance_.trace.store(log, std::memory_order_release); }

    // What an editor opened now shows: the newest code (the one playing, or
    // one swap() offered), and the channel its Shared members arrive by.
    const std::shared_ptr<const D::Code>& editorCode() const { return editorCode_; }
    const std::shared_ptr<SharedChannel>& editorChannel() const { return editorChannel_; }

    // Offers a new build of the script, with the same parameters, to
    // replace the running one at the start of a block. Main thread, after
    // the first prepare. False, offering nothing, when its prepare() reports
    // another latency than the host was told: that needs a restart.
    bool swap(std::shared_ptr<const D::Code> code, const ParamValues& values) {
        auto next =
            std::make_unique<Generation>(code, *table_, definition_->properties(), &instance_);
        load(*next, values, true);
        // Prepared here while the audio thread runs the current code, so
        // against a record of its own -- the same rate and sizes, and voices
        // that only count -- that its hooks' budget and setLatency() write;
        // the real one from the swap on. What it asks of the voices waits
        // for the swap.
        D::Instance own;
        own.sampleRate = instance_.sampleRate;
        own.maxBlockSize = instance_.maxBlockSize;
        own.channels = instance_.channels;
        own.offline = offlineNow_.load(std::memory_order_relaxed);
        PreparingBank preparing(voiceCount_, next->preparedVoices);
        if (instrument_) own.voices = &preparing;
        next->bindInstance(&own);
        next->prepare(&own);
        next->bindInstance(&instance_);
        const int latency = own.latency >= 0 ? own.latency : std::max(0, code->info().latency);
        if (latency != latencySamples()) return false;
        editorCode_ = code;
        editorChannel_ = next->channel();
        delete incoming_.exchange(next.release(), std::memory_order_acq_rel);
        return true;
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
        running_ = &g;
        D::CurrentScope scope(&instance_);
        instance_.budget = kBudgetPerHook;
        if (auto hook = g.program().update()) hook(&instance_, g.self());
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
            running_ = &g;
            D::CurrentScope scope(&instance_);
            instance_.budget = kBudgetPerFrame * std::max(block.frames, 1);
            if (auto hook = g.program().process()) hook(&instance_, g.self(), &io);
            running_ = current_.get();
        }
        for (uint16_t i : table_->smoothed())
            if (S::Param* p = g.param(i)) D::ParamBinding::setGlide(*p, nullptr, 0);
    }

    // A hook of the generation fading out. What it observes is not the new
    // code's, so the status goes back to what it was; budget it spends
    // stops it for the rest of the fade, not the plugin.
    template <class Hook>
    void runFading(Hook&& hook) {
        D::Status& s = instance_.status;
        constexpr auto relaxed = std::memory_order_relaxed;
        const uint32_t range = s.outOfRange.load(relaxed),
                       rangeLine = s.outOfRangeLine.load(relaxed),
                       budget = s.budgetExceeded.load(relaxed),
                       budgetLine = s.budgetLine.load(relaxed);
        const int tail = instance_.tail;  // nor is the tail it would set
        hook();
        instance_.tail = tail;
        if (s.budgetExceeded.load(relaxed) != budget) fadingStopped_ = true;
        s.outOfRange.store(range, relaxed);
        s.outOfRangeLine.store(rangeLine, relaxed);
        s.budgetExceeded.store(budget, relaxed);
        s.budgetLine.store(budgetLine, relaxed);
    }

    void adopt(Generation* next) {
        // Old code its budget stopped stays silent through the fade.
        const bool stopped = stopped_;
        instance_.status.clear();
        budgetSeen_ = 0;
        // Preserve what the replacement set while preparing on its own record.
        instance_.tail = next->preparedTail;
        // Its values are the running one's; it starts from them, snapped.
        for (size_t i = 0; i < next->paramCount() && i < current_->paramCount(); ++i)
            if (S::Param* p = next->param(i); p && current_->param(i))
                D::ParamBinding::setValue(*p, current_->param(i)->value(), true);
        {
            running_ = next;
            D::CurrentScope scope(&instance_);
            instance_.budget = kBudgetPerHook;
            if (auto hook = next->program().update()) hook(&instance_, next->self());
        }
        if (fading_) retire(std::move(fading_));
        fading_ = std::move(current_);
        fadingStopped_ = stopped;
        current_.reset(next);
        if (instrument_) {
            const Generation::VoiceSettings& v = current_->preparedVoices;
            if (v.mode >= 0) pendingMode_ = v.mode;
            if (!std::isnan(v.glideMs)) voices_.setGlide(v.glideMs);
            if (!std::isnan(v.bendRange)) voices_.setBendRange(v.bendRange);
            if (v.limit > 0) pendingLimit_ = v.limit;
        }
        // The files are the new code's now; the old lets go of them.
        bindFiles(*current_);
        if (fading_)
            for (size_t i = 0; i < files_.size(); ++i) fading_->bindFile(i, nullptr);
        fadeDone_ = 0;
        stopped_ = false;
        running_ = current_.get();
        checkBudget();
        // The voices sounding carry on in the new code: each starts again,
        // and a released one is released again.
        if (instrument_)
            for (int v = 0; v < voiceCount_; ++v)
                if (voices_[v].active) {
                    startVoice(v, true);  // the same note: its expressions stay
                    if (!voices_[v].held) voiceRelease(v);
                }
    }

    // A new note on voice v: its expressions neutral, its modulation none,
    // its random value drawn.
    void noteBegins(int v) {
        expression_[static_cast<size_t>(v)] = {1.0, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0};
        for (auto& amounts : polyAmount_) amounts[static_cast<size_t>(v)] = 0.0;
        // PCG32's step, as the script's Random takes it.
        random_ = random_ * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto bits = static_cast<uint32_t>(((random_ >> 18u) ^ random_) >> 27u);
        const auto turn = static_cast<uint32_t>(random_ >> 59u);
        const uint32_t word = (bits >> turn) | (bits << ((32u - turn) & 31u));
        noteRandom_[static_cast<size_t>(v)] = word / 2147483648.0 - 1.0;
    }

    void bindFiles(Generation& g) {
        for (size_t i = 0; i < files_.size(); ++i) g.bindFile(i, files_[i]);
    }

    // A voice hook: the script runs inside its instance, on a budget.
    template <class Call>
    void callVoice(Call&& call) {
        if (stopped_) return;
        {
            running_ = current_.get();
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
    // And a limit: lowering it stops the voices above it.
    void applyVoiceLimit() {
        if (pendingLimit_ < 0 || !instrument_) return;
        const int limit = std::clamp(pendingLimit_, 1, voiceCount_);
        pendingLimit_ = -1;
        if (limit != voices_.count()) voices_.setCount(limit, *this);
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
            const double hosts =
                modulatedPlain(p_.table_->at(static_cast<size_t>(slot)), value,
                               p_.polyAmount_[static_cast<size_t>(k)][static_cast<size_t>(v)]);
            return p_.routes_.routed(slot, v, hosts);
        }
        void finished(int v) override {
            if (!p_.fadingNow_) p_.voices_.finished(v, p_);
        }
        void setMode(S::VoiceMode mode) override { p_.pendingMode_ = static_cast<int>(mode); }
        void setGlide(double ms) override { p_.voices_.setGlide(ms); }
        void setBendRange(double semitones) override { p_.voices_.setBendRange(semitones); }
        void setVoiceLimit(int voices) override { p_.pendingLimit_ = voices; }

      private:
        ScriptProcessor& p_;
    };

    // A replacement's voices while its prepare() runs on the main thread:
    // as many as the running code has, none sounding; what it sets is kept
    // for the swap.
    class PreparingBank final : public D::VoiceBank {
      public:
        PreparingBank(int count, Generation::VoiceSettings& settings)
            : count_(count), settings_(settings) {}
        int count() const override { return count_; }
        bool playing(int) const override { return false; }
        int key(int) const override { return 0; }
        int channel(int) const override { return 0; }
        double velocity(int) const override { return 0.0; }
        double pitch(int, int) const override { return 0.0; }
        double expression(int, int which) const override {
            return which == 0 ? 1.0 : which == 1 ? 0.5 : 0.0;  // as a new note's
        }
        double modulated(int, int, double value) const override { return value; }
        void finished(int) override {}
        void setMode(S::VoiceMode mode) override { settings_.mode = static_cast<int>(mode); }
        void setGlide(double ms) override { settings_.glideMs = ms; }
        void setBendRange(double semitones) override { settings_.bendRange = semitones; }
        void setVoiceLimit(int voices) override { settings_.limit = voices; }

      private:
        int count_;
        Generation::VoiceSettings& settings_;
    };

    // The patch's routes, as Param::routed() and forVoice() read them.
    class Routes final : public D::RouteHost {
      public:
        explicit Routes(ScriptProcessor& p) : p_(p) {}

        double routed(int slot, int v, double value) const override {
            if (slot < 0 || static_cast<size_t>(slot) >= count_.size()) return value;
            const int n = count_[static_cast<size_t>(slot)];
            if (n == 0) return value;
            double sum = 0.0;
            for (int k = first_[static_cast<size_t>(slot)], end = k + n; k < end; ++k) {
                const Route& r = table_[static_cast<size_t>(k)];
                double x = p_.sourceValue(r.source, v);
                if (r.via >= 0) x *= p_.sourceValue(r.via, v);
                sum += r.amount * x;
            }
            const Reach& reach = reach_[static_cast<size_t>(slot)];
            const double moved = reach.octaves ? value * std::exp2(sum) : value + sum * reach.scale;
            return std::isfinite(moved) ? std::clamp(moved, reach.low, reach.high) : value;
        }

        // Main thread, once: each parameter's reach, room for its routes.
        void setUp(const ParamTable& table, const Meaning& meaning) {
            count_.assign(table.count(), 0);
            first_.assign(table.count(), 0);
            reach_.assign(table.count(), Reach{});
            for (size_t i = 0; i < table.count(); ++i) {
                const ParamDef& p = table.at(i);
                const ParamMeaning& pm = meaning.params[i];
                Reach& r = reach_[i];
                r.octaves = routeInOctaves(p);
                r.scale = routeScale(p);
                r.low = p.min;
                r.high = p.max;
                if (pm.routeMin < pm.routeMax) {
                    r.low = pm.routeMin * r.scale;
                    r.high = pm.routeMax * r.scale;
                }
            }
        }

        // Audio thread: the property's routes, in place of the table's.
        // A route naming what the plugin lacks is left out (the codec wrote
        // the text, so there are none).
        void load(std::string_view text, const Meaning& meaning) {
            std::fill(count_.begin(), count_.end(), 0);
            size_t used = 0;
            auto source = [&](std::string_view key) {
                for (size_t s = 0; s < meaning.sources.size(); ++s)
                    if (meaning.sources[s].key == key) return static_cast<int>(s);
                return -1;
            };
            forEachRoute(text, [&](const RouteLine& line) {
                if (used == table_.size()) return;
                int dest = -1;
                for (size_t i = 0; i < meaning.params.size(); ++i)
                    if (meaning.params[i].routable != Routable::No &&
                        meaning.params[i].key == line.destination)
                        dest = static_cast<int>(i);
                const int from = source(line.source);
                const int via = line.via.empty() ? -1 : source(line.via);
                if (dest < 0 || from < 0 || (!line.via.empty() && via < 0)) return;
                table_[used++] = Route{from, via, dest, line.amount};
            });
            // By destination, in the order they came (a stable insertion
            // sort: at most kMaxRoutes).
            for (size_t i = 1; i < used; ++i)
                for (size_t j = i; j > 0 && table_[j - 1].dest > table_[j].dest; --j)
                    std::swap(table_[j - 1], table_[j]);
            for (size_t k = 0; k < used; ++k) {
                const auto d = static_cast<size_t>(table_[k].dest);
                if (count_[d]++ == 0) first_[d] = static_cast<int>(k);
            }
        }

      private:
        struct Route {
            int source = 0, via = -1, dest = 0;
            double amount = 0.0;
        };
        // How the routes move a parameter's plain value, and how far.
        struct Reach {
            bool octaves = false;
            double scale = 1.0, low = 0.0, high = 1.0;
        };
        ScriptProcessor& p_;
        std::array<Route, kMaxRoutes> table_{};
        std::vector<int> count_, first_;  // by parameter table index
        std::vector<Reach> reach_;
    };

    void setUpRoutes() {
        const Meaning& meaning = definition_->meaning();
        if (meaning.routes < 0) return;
        routes_.setUp(*table_, meaning);
        instance_.routes = &routes_;
        const auto& own = definition_->code()->sources();
        scriptSources_ = static_cast<int>(own.size());
        for (const D::SourceModel& s : own) sourceOffsets_.push_back(s.offset);
        resetControllers();
    }
    void loadRoutes(std::string_view text) { routes_.load(text, definition_->meaning()); }

    // Source `k` (the meaning's order: the script's, then an instrument's
    // own) for voice v; -1 asks for the value shared by every voice, which a
    // per-voice source does not have.
    double sourceValue(int k, int v) const {
        if (k < scriptSources_) {
            const Generation* g = running_ ? running_ : current_.get();
            const auto* s =
                reinterpret_cast<const S::Source*>(static_cast<const unsigned char*>(g->self()) +
                                                   sourceOffsets_[static_cast<size_t>(k)]);
            if (v < 0)
                return definition_->meaning().sources[static_cast<size_t>(k)].perVoice ? 0.0
                                                                                       : s->get(0);
            return s->get(v);
        }
        const bool voice = v >= 0 && v < voiceCount_;
        const auto at = static_cast<size_t>(voice ? v : 0);
        switch (k - scriptSources_) {  // script::detail::kInstrumentSources' order
            case 0:                    // velocity
                return voice ? voices_[v].velocity : 0.0;
            case 1:  // key
                return voice ? std::clamp((voices_[v].note.key - 60) / 60.0, -1.0, 1.0) : 0.0;
            case 2:  // random
                return voice ? noteRandom_[at] : 0.0;
            case 3:  // pressure
                return voice ? expression_[at][6] : 0.0;
            case 4:  // slide
                return voice ? expression_[at][5] : 0.0;
            case 5:  // modwheel
                return controllers_[1];
            case 6:  // breath
                return controllers_[2];
            case 7:  // expression
                return controllers_[11];
            case 8:
                return aftertouch_;
            case 9:
                return pitchBend_;
            default:
                return 0.0;
        }
    }

    // The controllers at rest, and the notes' random values from the start.
    void resetControllers() {
        controllers_.fill(0.0);
        aftertouch_ = pitchBend_ = 0.0;
        random_ = 0x853c49e6748fea9bULL;
        noteRandom_.fill(0.0);
    }

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
            instance_.status.silent.store(true, std::memory_order_relaxed);
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
    // The main thread's: what an editor opened now shows.
    std::shared_ptr<const D::Code> editorCode_;
    std::shared_ptr<SharedChannel> editorChannel_;
    std::unique_ptr<Generation> current_;
    std::unique_ptr<Generation> fading_;
    std::vector<D::FileContents*> files_;  // by property index: the framework's, lent
    std::atomic<Generation*> incoming_{nullptr};
    Ring<Generation*, 3> retired_;
    std::vector<std::vector<double>> fadeBuffer_;
    std::vector<double*> fadePointers_;
    std::vector<std::vector<double>> fadeAuxBuffer_;
    std::vector<double*> fadeAuxPointers_;
    std::vector<double* const*> fadeAuxBuses_;
    int fadeLength_ = 480, fadeDone_ = 0;
    double spare_ = 0.0;
    bool stopped_ = false;
    bool fadingStopped_ = false;  // the old code ran out of budget: it fades from silence
    uint32_t budgetSeen_ = 0;
    int latency_ = 0;
    uint32_t tail_ = 0;
    // An instrument's.
    bool instrument_ = false;
    int voiceCount_ = kDefaultVoices;
    Voices voices_;
    Bank bank_{*this};
    int pendingMode_ = -1, pendingLimit_ = -1;
    int endFrame_ = 0;
    // Routes, and what their sources read: which generation's hook is
    // running, where its Source members are, the controllers last received
    // and each note's random value.
    Routes routes_{*this};
    const Generation* running_ = nullptr;
    int scriptSources_ = 0;
    std::vector<int> sourceOffsets_;
    std::array<double, 128> controllers_{};
    double aftertouch_ = 0.0, pitchBend_ = 0.0;
    uint64_t random_ = 0;
    std::array<double, kMaxVoices> noteRandom_{};
    // Each voice's note expressions (CLAP's order), and per poly-modulated
    // parameter (polySlot_, by table index) each voice's modulation.
    std::array<std::array<double, 7>, kMaxVoices> expression_{};
    std::vector<int> polySlot_;
    std::vector<std::array<double, kMaxVoices>> polyAmount_;
    bool fadingNow_ = false;
    // block.offline as the audio thread last saw it, for swap()'s prepare.
    std::atomic<bool> offlineNow_{false};
};

}  // namespace supple::suite
