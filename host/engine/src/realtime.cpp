#include "supple/engine/realtime.h"

#include <algorithm>
#include <chrono>

#include "plan.h"

namespace supple::engine {

// --- dummy backend -----------------------------------------------------------------

DummyBackend::DummyBackend(Options options) : options_(options) {}

DummyBackend::~DummyBackend() { stop(); }

Status DummyBackend::start(const BackendConfig& config, Callback callback) {
    if (running_) return Error("backend.running", "the backend is already running");
    config_ = config;
    callback_ = std::move(callback);
    outL_.assign(config.blockSize, 0.0f);
    outR_.assign(config.blockSize, 0.0f);
    capture_.sampleRate = config.sampleRate;
    capture_.channels.assign(
        2, std::vector<float>(static_cast<std::size_t>(options_.captureFrames), 0.0f));
    captured_ = 0;
    cycles_ = 0;
    running_ = true;
    if (options_.mode == Mode::Thread) {
        thread_ = std::thread([this] {
            const auto period =
                std::chrono::duration<double>(config_.blockSize / config_.sampleRate);
            auto next = std::chrono::steady_clock::now();
            while (running_.load()) {
                cycle();
                if (options_.paced) {
                    next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
                    std::this_thread::sleep_until(next);
                }
            }
        });
    }
    return okStatus();
}

void DummyBackend::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void DummyBackend::run(std::int64_t n) {
    for (std::int64_t i = 0; i < n && running_; ++i) cycle();
}

void DummyBackend::cycle() {
    float* out[2] = {outL_.data(), outR_.data()};
    callback_(nullptr, out, config_.blockSize);
    const auto room = std::min<std::int64_t>(config_.blockSize, options_.captureFrames - captured_);
    if (room > 0) {
        std::copy_n(outL_.begin(), room, capture_.channels[0].begin() + captured_);
        std::copy_n(outR_.begin(), room, capture_.channels[1].begin() + captured_);
        captured_ += room;
    }
    cycles_.fetch_add(1);
}

// --- engine --------------------------------------------------------------------------

Engine::Engine(InstancePool& plugins, MediaCache& media) : plugins_(plugins), media_(media) {
    processing_.reserve(kMaxInstances);
}

Engine::~Engine() { stop(); }

Status Engine::start(Backend& backend, const BackendConfig& config) {
    if (backend_) return Error("engine.running", "the engine is already running");
    config_ = config;
    shutDown_ = false;
    SUPPLE_TRY(backend.start(config, [this](const float* const* in, float* const* out,
                                            std::uint32_t n) { cycle(in, out, n); }));
    backend_ = &backend;
    return okStatus();
}

void Engine::stop() {
    if (!backend_) return;
    // Ask the audio thread to stop processing every instance, then stop the
    // backend. A manual backend has no thread: its next run() would do it,
    // so stop processing here (no cycle is running).
    commands_.push({Command::Op::Shutdown, 0});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (backend_->ownsThread() && !shutDown_.load() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    backend_->stop();
    if (!shutDown_.load()) {
        Command c{};
        while (commands_.pop(c)) {
        }
        for (host::Instance* in : processing_) in->stopProcessing();
    }
    processing_.clear();
    backend_ = nullptr;
    plan_ = nullptr;
    handoff_.collect();
}

Status Engine::load(const model::Session& session, const std::filesystem::path& projectDir) {
    if (!backend_) return Error("engine.stopped", "start the engine first");
    if (session.project->sampleRate != config_.sampleRate)
        return Error("engine.sample_rate",
                     "the project is at " + std::to_string(session.project->sampleRate) +
                         " Hz, the device at " + std::to_string(config_.sampleRate),
                     {}, "restart the engine at the project rate");
    auto compiled = detail::compile(session, plugins_, 0.0, config_.blockSize);
    if (!compiled) return std::move(compiled).error();
    auto plan = std::make_unique<detail::Plan>(std::move(*compiled));
    plan->projectDir = projectDir;
    plan->media = &media_;
    SUPPLE_TRY(detail::prepare(*plan, session));
    if (plan->instances.size() > kMaxInstances)
        return Error("engine.too_many_plugins",
                     "more than " + std::to_string(kMaxInstances) + " plugin instances");
    outputLatency_ = static_cast<std::uint32_t>(plan->outputLatency);
    warnings_ = plan->warnings;
    latest_ = std::make_shared<const model::Session>(session);
    prunePending_ = true;
    handoff_.publish(std::move(plan));
    return okStatus();
}

void Engine::play() { commands_.push({Command::Op::Play, 0}); }
void Engine::pause() { commands_.push({Command::Op::Pause, 0}); }
void Engine::seek(double seconds) {
    commands_.push(
        {Command::Op::Seek, time::samplesAt(std::max(0.0, seconds), config_.sampleRate)});
}

TransportState Engine::transport() const {
    TransportState t;
    t.playing = reportedPlaying_.load();
    t.sample = reportedPos_.load();
    t.seconds = static_cast<double>(t.sample) / config_.sampleRate;
    return t;
}

void Engine::service() {
    handoff_.collect();
    if (prunePending_ && latest_ &&
        handoff_.adoptedGeneration() == handoff_.publishedGeneration()) {
        plugins_.prune(*latest_);  // the audio thread stopped processing removed instances
        prunePending_ = false;
    }
}

void Engine::adopt(detail::Plan* next) SUPPLE_RT {
    // Stop what the new plan no longer uses, start what it adds, and remember
    // the new set (both lists are sorted; capacity was checked in load()).
    for (host::Instance* in : processing_) {
        const bool kept =
            next && std::binary_search(next->instances.begin(), next->instances.end(), in);
        if (!kept) in->stopProcessing();
    }
    processing_.clear();
    if (!next) return;
    for (host::Instance* in : next->instances) {
        if (!in->processing()) (void)in->startProcessing();
        processing_.push_back(in);  // within the reserved capacity
    }
}

void Engine::resetInstances(detail::Plan* plan) SUPPLE_RT {
    if (plan)
        for (host::Instance* in : plan->instances) in->reset();
}

void Engine::cycle(const float* const*, float* const* out, std::uint32_t frames) SUPPLE_RT {
    detail::Plan* plan = handoff_.acquire();
    if (plan != plan_) {
        adopt(plan);
        plan_ = plan;
        if (plan) detail::locate(*plan, pos_);
    }
    Command c{};
    while (commands_.pop(c)) {
        switch (c.op) {
            case Command::Op::Play:
                playing_ = true;
                break;
            case Command::Op::Pause:
                playing_ = false;
                resetInstances(plan);
                break;
            case Command::Op::Seek:
                pos_ = c.sample;
                if (plan) {
                    detail::locate(*plan, pos_);
                    resetInstances(plan);
                }
                break;
            case Command::Op::Shutdown:
                for (host::Instance* in : processing_) in->stopProcessing();
                processing_.clear();
                playing_ = false;
                shutDown_.store(true);
                break;
        }
    }
    const bool run = plan && playing_ && !shutDown_.load() && frames <= plan->block;
    if (run && detail::processBlock(*plan, pos_, frames)) {
        const detail::Node& master = plan->nodes.back();
        std::copy_n(master.post.l.begin(), frames, out[0]);
        std::copy_n(master.post.r.begin(), frames, out[1]);
        pos_ += frames;
    } else {
        if (run) processErrors_.fetch_add(1);
        std::fill_n(out[0], frames, 0.0f);
        std::fill_n(out[1], frames, 0.0f);
    }
    reportedPos_.store(pos_);
    reportedPlaying_.store(playing_);
}

}  // namespace supple::engine
