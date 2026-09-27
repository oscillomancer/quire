#include "supple/engine/realtime.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

#include "plan.h"
#include "workers.h"

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
            float* out[2] = {outL_.data(), outR_.data()};
            callback_(nullptr, out, 0);  // the final callback (Backend::stop contract)
        });
    }
    return okStatus();
}

void DummyBackend::stop() {
    const bool wasRunning = running_.exchange(false);
    if (thread_.joinable()) {
        thread_.join();
    } else if (wasRunning && callback_) {
        float* out[2] = {outL_.data(), outR_.data()};
        callback_(nullptr, out, 0);  // Manual: the caller is the audio thread
    }
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

// --- the anticipation ring ---------------------------------------------------------------

namespace detail {

// One render block of master output, tagged with the transport epoch it
// belongs to.
struct Chunk {
    std::uint64_t epoch = 0;
    std::int64_t start = 0;  // plan sample of the first frame
    std::uint32_t frames = 0;
    std::vector<float> l, r;
};

// Single producer (the renderer), single consumer (the audio callback).
struct ChunkRing {
    std::vector<Chunk> chunks;
    std::size_t mask = 0;
    alignas(64) std::atomic<std::size_t> write{0};
    alignas(64) std::atomic<std::size_t> read{0};

    ChunkRing(std::size_t capacity, std::uint32_t frames) {
        chunks.resize(std::bit_ceil(std::max<std::size_t>(capacity, 4)));
        mask = chunks.size() - 1;
        for (Chunk& c : chunks) {
            c.l.assign(frames, 0.0f);
            c.r.assign(frames, 0.0f);
        }
    }
    // Producer: the next free chunk, or null when the ring is full.
    Chunk* slot() {
        const std::size_t w = write.load(std::memory_order_relaxed);
        return w - read.load(std::memory_order_acquire) > mask ? nullptr : &chunks[w & mask];
    }
    void publish() {
        write.store(write.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    // Consumer.
    const Chunk* front() const SUPPLE_RT {
        const std::size_t r = read.load(std::memory_order_relaxed);
        return r == write.load(std::memory_order_acquire) ? nullptr : &chunks[r & mask];
    }
    void pop() SUPPLE_RT {
        read.store(read.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
};

}  // namespace detail

// --- engine --------------------------------------------------------------------------

Engine::Engine(InstancePool& plugins, MediaCache& media, EngineOptions options)
    : plugins_(plugins), media_(media), options_(options) {
    options_.renderBlock = std::clamp<std::uint32_t>(options_.renderBlock, 64, 8192);
    options_.renderAheadMs = std::clamp(options_.renderAheadMs, 10.0, 2000.0);
    processing_.reserve(kMaxInstances);
}

Engine::~Engine() { stop(); }

Status Engine::start(Backend& backend, const BackendConfig& config) {
    if (backend_) return Error("engine.running", "the engine is already running");
    if (config.outputs < 2 || config.blockSize == 0)
        return Error("engine.config", "the engine needs a block size and two outputs");
    shutDown_ = false;
    shutdownRequested_ = false;
    if (options_.anticipative) {
        // Allocated before the backend starts: its callbacks read the ring at
        // once. Room for the render-ahead amount at up to 192 kHz.
        const auto chunks = static_cast<std::size_t>(
            std::ceil(options_.renderAheadMs / 1000.0 * 192000.0 / options_.renderBlock));
        ring_ = std::make_unique<detail::ChunkRing>(chunks + 2, options_.renderBlock);
        rtEpoch_ = epoch_.load();
        rtOffset_ = 0;
        rtPrimed_ = false;
    }
    auto started = backend.start(config, [this](const float* const* in, float* const* out,
                                                std::uint32_t n) { cycle(in, out, n); });
    if (!started) {
        ring_.reset();
        return started;
    }
    config_ = backend.config();  // what the device granted (e.g. its rate)
    backend_ = &backend;
    if (options_.anticipative) {
        aheadFrames_ = std::max<std::int64_t>(
            2 * options_.renderBlock,
            static_cast<std::int64_t>(options_.renderAheadMs / 1000.0 * config_.sampleRate));
        aheadFrames_ = std::min<std::int64_t>(
            aheadFrames_, static_cast<std::int64_t>(ring_->mask - 1) * options_.renderBlock);
        workers_ = std::make_unique<detail::Workers>(
            options_.workers < 0 ? detail::Workers::defaultHelpers()
                                 : static_cast<unsigned>(std::min(options_.workers, 64)));
        // The renderer's starting epoch is taken here, not on its thread: a
        // seek or pause issued before the thread runs must still be seen.
        const std::uint64_t first = epoch_.load(std::memory_order_acquire);
        epochStartLocal_ = pos_;
        epochStart_.store(pos_);
        renderedTo_.store(pos_);
        epochDone_.store(first, std::memory_order_release);
        rendererQuit_ = false;
        renderer_ = std::thread([this, first] { rendererLoop(first); });
    }
    return okStatus();
}

void Engine::stop() {
    if (!backend_) return;
    // Silence from the next cycle on, then stop the backend: its final
    // zero-frame callback stops every plugin on the audio thread.
    shutdownRequested_.store(true, std::memory_order_release);
    stoppedOnAudioThread_ = false;
    if (renderer_.joinable()) {  // the renderer stops every plugin as it ends
        rendererQuit_.store(true, std::memory_order_release);
        renderer_.join();
    }
    backend_->stop();
    if (!shutDown_.load()) {
        // A backend that broke its stop() contract has already joined its
        // thread, so nothing else touches the plugins: recover here.
        for (host::Instance* in : processing_) in->stopProcessing();
    }
    processing_.clear();
    backend_ = nullptr;
    plan_ = nullptr;
    handoff_.collect();
    workers_.reset();
    ring_.reset();  // no callbacks after backend_->stop()
}

Status Engine::load(const model::Session& session, const std::filesystem::path& projectDir) {
    if (!backend_) return Error("engine.stopped", "start the engine first");
    if (session.project->sampleRate != config_.sampleRate)
        return Error("engine.sample_rate",
                     "the project is at " + std::to_string(session.project->sampleRate) +
                         " Hz, the device at " + std::to_string(config_.sampleRate),
                     {}, "run the project and the device at the same rate");
    // Instances the compile replaces may still play in plans up to this one.
    plugins_.setGeneration(publishedGen_ + 1);
    auto compiled = detail::compile(
        session, plugins_, 0.0, options_.anticipative ? options_.renderBlock : config_.blockSize);
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
    plugins_.setGeneration(publishedGen_ + 1);  // later replacements: after this plan
    return okStatus();
}

void Engine::play() { wantPlaying_.store(true, std::memory_order_release); }
void Engine::pause() {
    wantPlaying_.store(false, std::memory_order_release);
    epoch_.fetch_add(1, std::memory_order_release);  // anticipative: drop what is rendered
}
void Engine::seek(double seconds) {
    seekSample_.store(time::samplesAt(std::max(0.0, seconds), config_.sampleRate),
                      std::memory_order_relaxed);
    seekSeq_.fetch_add(1, std::memory_order_release);
    epoch_.fetch_add(1, std::memory_order_release);
}

std::int64_t Engine::playedPosition(std::uint64_t epoch, std::int64_t epochStart) const {
    return rtPosEpoch_.load(std::memory_order_acquire) == epoch ? rtPos_.load() : epochStart;
}

TransportState Engine::transport() const {
    TransportState t;
    t.playing = reportedPlaying_.load();
    t.sample = options_.anticipative ? playedPosition(epoch_.load(), epochStart_.load())
                                     : reportedPos_.load();
    t.seconds = static_cast<double>(t.sample) / config_.sampleRate;
    return t;
}

void Engine::awaitTransport(double timeoutSeconds) {
    if (!backend_) return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    auto waitUntil = [&](auto done) {
        while (!done() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    };
    if (options_.anticipative) {
        const std::uint64_t e = epoch_.load();
        const bool want = wantPlaying_.load();
        waitUntil([&] { return epochDone_.load() >= e && reportedPlaying_.load() == want; });
    } else if (backend_->ownsThread()) {
        const std::uint64_t before = cycles_.load();  // the first may have begun before
        waitUntil([&] { return cycles_.load() >= before + 2; });
    }
}

bool Engine::awaitRendered(std::int64_t frames, double timeoutSeconds) {
    if (!ring_) return true;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    const std::uint64_t e = epoch_.load();
    const bool want = wantPlaying_.load();
    const std::int64_t goal = std::min(frames, aheadFrames_);
    for (;;) {
        if (epochDone_.load() >= e && reportedPlaying_.load() == want) {
            if (!want) return true;
            const std::int64_t ahead =
                renderedTo_.load(std::memory_order_acquire) - playedPosition(e, epochStart_.load());
            if (ahead >= goal) return true;
        }
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void Engine::service() {
    handoff_.collect();
    if (prunePending_ && latest_ && stoppedGen_.load(std::memory_order_acquire) >= publishedGen_) {
        // The audio side has adopted the newest plan and stopped what it dropped.
        plugins_.prune(*latest_, stoppedGen_.load(std::memory_order_acquire));
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
    if (frames == 0) {  // the backend's final callback: stop every plugin here
        if (!shutDown_.load()) {
            for (host::Instance* in : processing_) in->stopProcessing();
            processing_.clear();
            playing_ = false;
            stoppedOnAudioThread_ = true;
            shutDown_.store(true);
        }
        return;
    }
    if (ring_) {  // anticipative: the renderer has done the work
        pullAhead(out, frames);
        cycles_.fetch_add(1, std::memory_order_release);
        return;
    }
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
        stoppedOnAudioThread_ = true;
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
    cycles_.fetch_add(1, std::memory_order_release);
}

// --- anticipation (ADR-0012) -----------------------------------------------------------------

void Engine::pullAhead(float* const* out, std::uint32_t frames) SUPPLE_RT {
    const std::uint64_t e = epoch_.load(std::memory_order_acquire);
    if (e != rtEpoch_) {
        rtEpoch_ = e;
        rtPrimed_ = false;
        rtEpochSeen_.store(e, std::memory_order_release);
    }
    detail::ChunkRing& ring = *ring_;
    while (const detail::Chunk* c = ring.front()) {  // flushed by a seek or pause
        if (c->epoch >= e) break;
        ring.pop();
        rtOffset_ = 0;
    }
    std::uint32_t done = 0;
    while (done < frames) {
        const detail::Chunk* c = ring.front();
        if (!c || c->epoch != e) break;
        const std::uint32_t take = std::min(frames - done, c->frames - rtOffset_);
        std::copy_n(c->l.begin() + rtOffset_, take, out[0] + done);
        std::copy_n(c->r.begin() + rtOffset_, take, out[1] + done);
        done += take;
        rtOffset_ += take;
        rtPos_.store(c->start + rtOffset_, std::memory_order_relaxed);
        rtPosEpoch_.store(e, std::memory_order_release);
        if (rtOffset_ == c->frames) {
            ring.pop();
            rtOffset_ = 0;
        }
    }
    if (done < frames) {
        std::fill_n(out[0] + done, frames - done, 0.0f);
        std::fill_n(out[1] + done, frames - done, 0.0f);
        if (rtPrimed_ && wantPlaying_.load(std::memory_order_relaxed))
            misses_.fetch_add(1, std::memory_order_relaxed);
    }
    if (done > 0) rtPrimed_ = true;
}

void Engine::relocate(std::uint64_t previous, std::uint64_t epoch) {
    if (const std::uint64_t seq = seekSeq_.load(std::memory_order_acquire); seq != seekSeen_) {
        seekSeen_ = seq;
        pos_ = seekSample_.load(std::memory_order_relaxed);
    } else {
        // Paused: resume where the listener is. Once the callback has seen
        // the new epoch it plays nothing older, so its position is final. (A
        // manual backend's callback runs only when its caller says.)
        if (backend_->ownsThread()) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (rtEpochSeen_.load(std::memory_order_acquire) < epoch && !rendererQuit_.load() &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        pos_ = playedPosition(previous, epochStartLocal_);
    }
    if (plan_) {
        detail::locate(*plan_, pos_);
        resetInstances(plan_);
    }
    epochStartLocal_ = pos_;
    epochStart_.store(pos_);
    renderedTo_.store(pos_, std::memory_order_release);
    epochDone_.store(epoch, std::memory_order_release);
}

void Engine::rendererLoop(std::uint64_t seen) {
    const std::uint32_t block = options_.renderBlock;
    const auto nap = std::chrono::duration<double>(0.25 * block / config_.sampleRate);
    while (!rendererQuit_.load(std::memory_order_acquire)) {
        detail::Plan* plan = handoff_.acquire();
        if (plan != plan_) {  // edits take effect here: after what is already rendered
            adopt(plan);
            plan_ = plan;
            if (plan) {
                detail::locate(*plan, pos_);
                stoppedGen_.store(plan->generation, std::memory_order_release);
            }
        }
        if (const std::uint64_t e = epoch_.load(std::memory_order_acquire); e != seen) {
            relocate(seen, e);
            seen = e;
        }
        if (const bool want = wantPlaying_.load(std::memory_order_acquire); want != playing_) {
            playing_ = want;
            if (!want) resetInstances(plan_);  // no hanging notes
            reportedPlaying_.store(playing_);
        }
        const std::int64_t ahead = pos_ - playedPosition(seen, epochStartLocal_);
        detail::Chunk* c =
            playing_ && plan_ && !hold_.load() && ahead < aheadFrames_ ? ring_->slot() : nullptr;
        if (!c) {
            std::this_thread::sleep_for(playing_ ? nap : std::chrono::duration<double>(0.001));
            continue;
        }
        const bool ok = workers_->helpers() ? detail::processBlockOn(*workers_, *plan_, pos_, block)
                                            : detail::processBlock(*plan_, pos_, block);
        if (ok) {
            const detail::Node& master = plan_->nodes.back();
            std::copy_n(master.post.l.begin(), block, c->l.begin());
            std::copy_n(master.post.r.begin(), block, c->r.begin());
        } else {
            processErrors_.fetch_add(1);
            std::fill(c->l.begin(), c->l.end(), 0.0f);
            std::fill(c->r.begin(), c->r.end(), 0.0f);
        }
        c->epoch = seen;
        c->start = pos_;
        c->frames = block;
        ring_->publish();
        pos_ += block;
        renderedTo_.store(pos_, std::memory_order_release);
    }
    // The renderer is the plugins' audio thread: stop them here.
    for (host::Instance* in : processing_) in->stopProcessing();
    processing_.clear();
    playing_ = false;
    reportedPlaying_.store(false);
    stoppedOnAudioThread_ = true;
    shutDown_.store(true);
}

}  // namespace supple::engine
