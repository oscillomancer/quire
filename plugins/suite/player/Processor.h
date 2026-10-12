// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The player's DSP: a compiled script (ScriptDefinition.h) run through the
// framework's engine.
//
//   - update(): each Param member gets its parameter's plain value (what the
//     script reads), marked changed when it moved, and the script's update()
//     runs. process(): a smoothed() parameter's per-frame values come from
//     the framework's glide (Block::smoothed), then the script's process().
//     Meters (meter()) go to the host through setOutput().
//   - reset() returns every member to what prepare() left: a copy taken
//     after prepare(), copied back without allocating.
//   - Each hook runs with a budget of loop iterations. A script that spends
//     it is stopped where it was, and stays silent until its next prepare or
//     replacement; its status says where (Instance::status).
//   - A new compile of the same script, with the same parameters, replaces
//     the running one without the host noticing (swap()): prepared on the
//     main thread, taken at the start of a block, crossfaded over 10 ms. The
//     one it replaced goes back to the main thread to be freed (collect()).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "ScriptDefinition.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"
#include "core/Ring.h"
#include "supple/script_runtime.h"

namespace supple::player {

namespace S = supple::script;
namespace D = supple::script::detail;

// One compiled script's running struct, its post-prepare copy, and its
// Param members by parameter table index.
class Generation {
  public:
    Generation(std::shared_ptr<const scriptc::Program> program, const ParamTable& table,
               D::Instance* instance)
        : program_(std::move(program)), self_(program_->create()) {
        D::Instance::bind(*static_cast<S::Plugin*>(self_), instance);
        params_.assign(table.count(), nullptr);
        for (const scriptc::ParamModel& m : program_->params()) {
            const int i = table.indexOf(m.id);
            if (i < 0) continue;
            auto* p = reinterpret_cast<S::Param*>(static_cast<unsigned char*>(self_) + m.offset);
            params_[static_cast<size_t>(i)] = p;
            const ParamDef& def = table.at(static_cast<size_t>(i));
            D::ParamBinding::setRange(*p, def.min, def.is(pflag::kOffAtMin));
        }
    }
    ~Generation() {
        if (reset_) program_->destroy(reset_);
        program_->destroy(self_);
    }
    Generation(const Generation&) = delete;
    Generation& operator=(const Generation&) = delete;

    const scriptc::Program& program() const { return *program_; }
    void* self() const { return self_; }
    S::Param* param(size_t index) const { return params_[index]; }
    size_t paramCount() const { return params_.size(); }

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

  private:
    std::shared_ptr<const scriptc::Program> program_;
    void* self_;
    void* reset_ = nullptr;
    std::vector<S::Param*> params_;
};

class ScriptProcessor : public ProcessorBase<ScriptProcessor> {
  public:
    explicit ScriptProcessor(const DefinitionPtr& definition)
        : definition_(std::static_pointer_cast<const ScriptDefinition>(definition)),
          table_(&definition_->params()),
          current_(std::make_unique<Generation>(definition_->program(), *table_, &instance_)) {
        const scriptc::InfoModel& info = definition_->program()->info();
        latency_ = std::max(0, info.latency);
        tail_ = info.tail < 0 ? UINT32_MAX : static_cast<uint32_t>(info.tail);
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
        load(*current_, v, true);
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
    }

    void reset() {
        current_->reset();
        if (fading_) retire(std::move(fading_));
        stopped_ = false;
    }

    void process(Block& block) {
        if (Generation* next = incoming_.exchange(nullptr, std::memory_order_acq_rel)) adopt(next);
        if (stopped_) {
            silence(block);
            return;
        }
        if (fading_) {
            // The old generation plays on a copy of the input; the two meet
            // along a linear fade.
            for (size_t c = 0; c < fadePointers_.size() && static_cast<int>(c) < block.channels;
                 ++c)
                std::copy(block.data[c], block.data[c] + block.frames, fadePointers_[c]);
            Block copy = block;
            copy.data = fadePointers_.data();
            runProcess(*fading_, copy);
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
    }

    int latencySamples() const { return latency_; }
    uint32_t tailSamples() const { return tail_; }

    // --- the layer's (main thread) ---------------------------------------------------

    // The script's runtime record: its sample rate and block size, and its status.
    const D::Instance& instance() const { return instance_; }

    // Offers a new compile of the script, with the same parameters, to
    // replace the running one at the start of a block. Main thread, after
    // the first prepare.
    void swap(std::shared_ptr<const scriptc::Program> program, const ParamValues& values) {
        auto next = std::make_unique<Generation>(std::move(program), *table_, &instance_);
        load(*next, values, true);
        next->prepare(&instance_);
        delete incoming_.exchange(next.release(), std::memory_order_acq_rel);
    }

    // Frees what the audio thread let go of.
    void collect() {
        Generation* done = nullptr;
        while (retired_.pop(done)) delete done;
    }

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
        fadeDone_ = 0;
        stopped_ = false;
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
    static constexpr std::int64_t kBudgetPerHook = std::int64_t{1} << 26;
    static constexpr std::int64_t kBudgetPerFrame = std::int64_t{1} << 16;

    std::shared_ptr<const ScriptDefinition> definition_;
    const ParamTable* table_;
    D::Instance instance_;
    std::unique_ptr<Generation> current_;
    std::unique_ptr<Generation> fading_;
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
};

}  // namespace supple::player

namespace product {
using Processor = supple::player::ScriptProcessor;
}  // namespace product
