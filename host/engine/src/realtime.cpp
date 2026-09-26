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
    shutdownRequested_ = false;
    SUPPLE_TRY(backend.start(config, [this](const float* const* in, float* const* out,
                                            std::uint32_t n) { cycle(in, out, n); }));
    backend_ = &backend;
    return okStatus();
}

void Engine::stop() {
    if (!backend_) return;
    // Ask the audio thread to stop processing every instance (a flag: never
    // dropped), then stop the backend. A manual backend has no thread: its
    // caller is the audio thread and no cycle is running, so stop here.
    shutdownRequested_.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (backend_->ownsThread() && !shutDown_.load() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    backend_->stop();
    if (!shutDown_.load()) {
        // No cycle acknowledged (manual backend, or a stalled device thread
        // that the backend has now joined): nothing else touches the plugins.
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
    plan->generation = ++publishedGen_;
    handoff_.publish(std::move(plan));
    return okStatus();
}

void Engine::play() { wantPlaying_.store(true, std::memory_order_release); }
void Engine::pause() { wantPlaying_.store(false, std::memory_order_release); }
void Engine::seek(double seconds) {
    seekSample_.store(time::samplesAt(std::max(0.0, seconds), config_.sampleRate),
                      std::memory_order_relaxed);
    seekSeq_.fetch_add(1, std::memory_order_release);
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
    if (prunePending_ && latest_ && stoppedGen_.load(std::memory_order_acquire) >= publishedGen_) {
        plugins_.prune(*latest_);  // the audio thread finished stopping removed instances
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
        // A plugin that refuses to start is left out (its stage passes audio
        // through, see detail::processBlock) and retried on the next plan.
        if (!in->processing() && !in->startProcessing()) {
            processErrors_.fetch_add(1);
            continue;
        }
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
        if (plan) {
            detail::locate(*plan, pos_);
            stoppedGen_.store(plan->generation, std::memory_order_release);
        }
    }
    if (shutdownRequested_.load(std::memory_order_acquire) && !shutDown_.load()) {
        for (host::Instance* in : processing_) in->stopProcessing();
        processing_.clear();
        playing_ = false;
        shutDown_.store(true);
    }
    if (const std::uint64_t seq = seekSeq_.load(std::memory_order_acquire); seq != seekSeen_) {
        seekSeen_ = seq;
        pos_ = seekSample_.load(std::memory_order_relaxed);
        if (plan) {
            detail::locate(*plan, pos_);
            resetInstances(plan);
        }
    }
    if (const bool want = wantPlaying_.load(std::memory_order_acquire); want != playing_) {
        playing_ = want;
        if (!want) resetInstances(plan);  // no hanging notes
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
