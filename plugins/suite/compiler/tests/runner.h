// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Test drivers: run a script's hooks on buffers, the way the player's glue
// does, both for a compiled Program (JIT or interpreter) and for the same
// script compiled as C++ (what a source export builds). The two must agree
// bit for bit.
#pragma once

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "supple/script.h"
#include "supple/script_compiler.hpp"
#include "supple/script_program.hpp"
#include "supple/script_runtime.h"

namespace supple::scripttest {

namespace S = supple::script;
namespace D = supple::script::detail;

// Audio in place: channels x frames.
struct Audio {
    std::vector<std::vector<double>> data;
    explicit Audio(int channels = 2, int frames = 0)
        : data(static_cast<size_t>(channels), std::vector<double>(static_cast<size_t>(frames))) {}
    int channels() const { return static_cast<int>(data.size()); }
    int frames() const { return data.empty() ? 0 : static_cast<int>(data[0].size()); }
    bool operator==(const Audio& o) const {
        if (data.size() != o.data.size()) return false;
        for (size_t c = 0; c < data.size(); ++c)
            if (data[c].size() != o.data[c].size() ||
                std::memcmp(data[c].data(), o.data[c].data(), data[c].size() * sizeof(double)) != 0)
                return false;
        return true;
    }
};

// The parts every runner shares: the instance record and Block assembly.
class RunnerBase {
  public:
    D::Instance instance;
    double spare = 0.0;
    // A sidechain input, as long as the audio and as wide, when set.
    const Audio* key = nullptr;

    // Runs process over `audio` in blocks of `block` frames.
    template <class Call>
    void blocks(Audio& audio, int block, Call&& call) {
        const int frames = audio.frames();
        std::vector<double*> pointers(static_cast<size_t>(audio.channels()));
        std::vector<const double*> keys(static_cast<size_t>(audio.channels()));
        for (int start = 0; start < frames; start += block) {
            const int n = std::min(block, frames - start);
            for (int c = 0; c < audio.channels(); ++c) {
                pointers[static_cast<size_t>(c)] =
                    audio.data[static_cast<size_t>(c)].data() + start;
                if (key)
                    keys[static_cast<size_t>(c)] = key->data[static_cast<size_t>(c)].data() + start;
            }
            S::Block io;
            io.channels = audio.channels();
            io.frames = n;
            io.data_ = pointers.data();
            io.spare_ = &spare;
            if (key) {
                io.sidechainConnected = true;
                io.sidechain_ = keys.data();
                io.sidechainChannels_ = audio.channels();
            }
            call(io);
        }
    }
};

class JitRunner : public RunnerBase {
  public:
    explicit JitRunner(std::shared_ptr<const scriptc::Program> program, double sampleRate = 48000.0,
                       int maxBlock = 512, int channels = 2)
        : program_(std::move(program)) {
        instance.sampleRate = sampleRate;
        instance.maxBlockSize = maxBlock;
        instance.channels = channels;
        self_ = program_->create();
        auto* plugin = static_cast<S::Plugin*>(self_);
        D::Instance::bind(*plugin, &instance);
    }
    ~JitRunner() { program_->destroy(self_); }

    void* self() const { return self_; }
    // Sets parameter `index` to `physical` (a percent in percent).
    void set(size_t index, double physical, bool changed = true) {
        const auto& p = program_->params()[index];
        const double v = p.unit == scriptc::ParamModel::Unit::Percent ? physical * 0.01 : physical;
        D::ParamBinding::setValue(param(index), v, changed);
    }
    // Sets parameter `index` to the value the script reads (a percent as a fraction).
    void setScript(size_t index, double value, bool changed = true) {
        D::ParamBinding::setValue(param(index), value, changed);
    }
    void clearChanged() {
        for (size_t i = 0; i < program_->params().size(); ++i)
            D::ParamBinding::setValue(param(i), param(i).value(), false);
    }
    S::Param& param(size_t index) const {
        return *reinterpret_cast<S::Param*>(static_cast<unsigned char*>(self_) +
                                            program_->params()[index].offset);
    }
    void prepare() {
        D::CurrentScope scope(&instance);
        instance.budget = std::int64_t{1} << 40;
        if (auto f = program_->prepare()) f(&instance, self_);
    }
    void update() {
        D::CurrentScope scope(&instance);
        instance.budget = std::int64_t{1} << 40;
        if (auto f = program_->update()) f(&instance, self_);
        clearChanged();
    }
    void process(Audio& audio, int block = 512) {
        D::CurrentScope scope(&instance);
        blocks(audio, block, [&](S::Block& io) {
            instance.budget = std::int64_t{1} << 40;
            program_->process()(&instance, self_, &io);
        });
    }
    // The editor's hooks, building and drawing with these.
    void editor(D::EditorBuilder* builder) {
        S::Editor e;
        D::Layout::bind(e, builder);
        D::CurrentScope scope(&instance);
        instance.budget = std::int64_t{1} << 40;
        if (auto f = program_->editor()) f(&instance, self_, &e);
    }
    void draw(D::Painter* painter) {
        S::Canvas g;
        D::Layout::bind(g, painter);
        D::CurrentScope scope(&instance);
        instance.budget = std::int64_t{1} << 40;
        if (auto f = program_->draw()) f(&instance, self_, &g);
    }

  private:
    std::shared_ptr<const scriptc::Program> program_;
    void* self_ = nullptr;
};

// The same script compiled as C++: T is its plugin struct.
template <class T>
class NativeRunner : public RunnerBase {
  public:
    explicit NativeRunner(double sampleRate = 48000.0, int maxBlock = 512, int channels = 2) {
        instance.sampleRate = sampleRate;
        instance.maxBlockSize = maxBlock;
        instance.channels = channels;
        Collector collector;
        D::setRegistrar(&collector);
        plugin_ = std::make_unique<T>();
        D::setRegistrar(nullptr);
        params_ = collector.params;
        // Each Param's place in the table, as the glue binds it.
        for (size_t i = 0; i < params_.size(); ++i)
            D::ParamBinding::setSlot(*params_[i], static_cast<int>(i));
        D::Instance::bind(*plugin_, &instance);
    }
    T& plugin() { return *plugin_; }
    // In declaration order, as the compiler numbers them.
    void set(size_t index, double scriptValue, bool changed = true) {
        D::ParamBinding::setValue(*params_[index], scriptValue, changed);
    }
    void clearChanged() {
        for (S::Param* p : params_) D::ParamBinding::setValue(*p, p->value(), false);
    }
    void prepare() {
        D::CurrentScope scope(&instance);
        if constexpr (requires(T& t) { t.prepare(); }) plugin_->prepare();
    }
    void update() {
        D::CurrentScope scope(&instance);
        if constexpr (requires(T& t) { t.update(); }) plugin_->update();
        clearChanged();
    }
    void process(Audio& audio, int block = 512) {
        D::CurrentScope scope(&instance);
        blocks(audio, block, [&](S::Block& io) { plugin_->process(io); });
    }
    void editor(D::EditorBuilder* builder) {
        S::Editor e;
        D::Layout::bind(e, builder);
        D::CurrentScope scope(&instance);
        if constexpr (requires(T& t, S::Editor& x) { t.editor(x); }) plugin_->editor(e);
    }
    void draw(D::Painter* painter) {
        S::Canvas g;
        D::Layout::bind(g, painter);
        D::CurrentScope scope(&instance);
        if constexpr (requires(T& t, S::Canvas& x) { t.draw(x); }) plugin_->draw(g);
    }

  private:
    struct Collector : D::ParamRegistrar {
        std::vector<S::Param*> params;
        void declare(S::Param& p, const S::ParamSpec&) override { params.push_back(&p); }
    };
    std::unique_ptr<T> plugin_;
    std::vector<S::Param*> params_;
};

}  // namespace supple::scripttest
