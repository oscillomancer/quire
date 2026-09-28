#include "supple/engine/realtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>

#include "plan.h"
#include "recording.h"
#include "workers.h"

namespace supple::engine {

std::uint64_t midiPortMask(const std::vector<std::string>& ports, std::string_view device) {
    auto lower = [](std::string_view v) {
        std::string out(v);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    const std::string want = lower(device);
    std::uint64_t mask = 0;
    for (std::size_t i = 0; i < ports.size() && i < 64; ++i)
        if (want.empty() || want == "*" || lower(ports[i]).find(want) != std::string::npos)
            mask |= std::uint64_t{1} << i;
    return mask;
}

// --- dummy backend -----------------------------------------------------------------

DummyBackend::DummyBackend(Options options) : options_(options) {}

DummyBackend::~DummyBackend() { stop(); }

Status DummyBackend::start(const BackendConfig& config, Callback callback) {
    if (running_) return Error("backend.running", "the backend is already running");
    config_ = config;
    callback_ = std::move(callback);
    outL_.assign(config.blockSize, 0.0f);
    outR_.assign(config.blockSize, 0.0f);
    inputs_.assign(config.inputs, std::vector<float>(config.blockSize, 0.0f));
    inPtrs_.clear();
    for (auto& c : inputs_) inPtrs_.push_back(c.data());
    inputFrame_ = 0;
    midi_.clear();
    midi_.reserve(kMaxMidiInputEvents);
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
    if (options_.input && !inPtrs_.empty())
        options_.input(inPtrs_.data(), config_.inputs, config_.blockSize, inputFrame_);
    midi_.clear();
    if (options_.midi && config_.midiInputs) {
        options_.midi(midi_, config_.blockSize, inputFrame_);
        if (midi_.size() > kMaxMidiInputEvents) midi_.resize(kMaxMidiInputEvents);
        for (std::size_t i = 1; i < midi_.size(); ++i)  // stable, allocation-free
            for (std::size_t k = i; k > 0 && midi_[k].frame < midi_[k - 1].frame; --k)
                std::swap(midi_[k], midi_[k - 1]);
    }
    inputFrame_ += config_.blockSize;
    callback_(inPtrs_.empty() ? nullptr : inPtrs_.data(), out, config_.blockSize);
    const auto room = std::min<std::int64_t>(config_.blockSize, options_.captureFrames - captured_);
    if (room > 0) {
        std::copy_n(outL_.begin(), room, capture_.channels[0].begin() + captured_);
        std::copy_n(outR_.begin(), room, capture_.channels[1].begin() + captured_);
        captured_ += room;
    }
    cycles_.fetch_add(1);
}

// --- engine --------------------------------------------------------------------------

Engine::Engine(InstancePool& plugins, MediaCache& media, EngineOptions options)
    : plugins_(plugins), media_(media), options_(options) {
    options_.renderBlock = std::clamp<std::uint32_t>(options_.renderBlock, 64, 8192);
    options_.renderAheadMs = std::clamp(options_.renderAheadMs, 10.0, 2000.0);
    processing_.reserve(kMaxInstances);
    liveProcessing_.reserve(kMaxInstances);
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
        livePlan_ = nullptr;
        liveReady_ = false;
        callbackGen_.store(0);
    }
    deviceInputs_ =
        std::min<std::uint32_t>(config.inputs, 64);  // the callback's copy, set before it runs
    midiSource_ = &backend;  // before any callback (backend_ is set after start returns)
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
        streamLocal_ = 0;
        renderedStream_.store(0);
        loopR_ = readLoop();
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
    recorder_.store(nullptr, std::memory_order_release);
    backend_->stop();
    if (recorderOwned_) (void)recorderOwned_->finish(readLoop());  // no callback now: keep files
    recorderOwned_.reset();
    // A backend that broke its stop() contract has already joined its
    // thread, so nothing else touches the plugins: recover here.
    if (!shutDown_.load())
        for (host::Instance* in : processing_) in->stopProcessing();
    if (!liveProcessing_.empty()) stoppedOnAudioThread_ = false;
    for (host::Instance* in : liveProcessing_) in->stopProcessing();
    for (auto* list : {&processing_, &liveProcessing_}) {
        for (host::Instance* in : *list) in->setOwner(nullptr);
        list->clear();
    }
    backend_ = nullptr;
    plan_ = nullptr;
    livePlan_ = nullptr;
    handPlan_.store(nullptr);
    latestPlan_.store(nullptr);
    plans_.clear();  // nothing runs a plan now
    callbackGen_.store(0);
    workers_.reset();
    ring_.reset();  // no callbacks after backend_->stop()
}

Status Engine::load(const model::Session& session, const std::filesystem::path& projectDir) {
    if (!backend_) return Error("engine.stopped", "start the engine first");
    // Instances the compile replaces may still play in plans up to this one.
    plugins_.setGeneration(publishedGen_ + 1);
    auto compiled = detail::compile(
        session, plugins_, 0.0, options_.anticipative ? options_.renderBlock : config_.blockSize,
        config_.sampleRate);  // the device's rate, whatever the project's
    if (!compiled) return std::move(compiled).error();
    auto plan = std::make_unique<detail::Plan>(std::move(*compiled));
    plan->projectDir = projectDir;
    plan->media = &media_;
    SUPPLE_TRY(detail::prepare(*plan, session));
    if (std::any_of(plan->nodes.begin(), plan->nodes.end(),
                    [](const detail::Node& n) { return n.midiIn.has_value(); })) {
        const std::vector<std::string> ports = backend_->midiPorts();  // ADR-0016
        for (detail::Node& n : plan->nodes)
            if (n.midiIn) n.midiIn->ports = midiPortMask(ports, n.midiIn->device);
    }
    if (plan->instances.size() > kMaxInstances)
        return Error("engine.too_many_plugins",
                     "more than " + std::to_string(kMaxInstances) + " plugin instances");
    if (options_.anticipative && plan->anyLive) {
        // The renderer hands its frontier taps to the callback (ADR-0014).
        plan->split = true;
        const auto chunks = static_cast<std::size_t>(
            std::ceil(options_.renderAheadMs / 1000.0 * 192000.0 / options_.renderBlock));
        for (std::size_t i = 0; i < plan->frontier.size(); ++i)
            plan->frontierRings.push_back(
                std::make_unique<detail::ChunkRing>(chunks + 2, options_.renderBlock));
    }
    outputLatency_ = static_cast<std::uint32_t>(plan->outputLatency);
    warnings_ = plan->warnings;
    latest_ = std::make_shared<const model::Session>(session);
    prunePending_ = true;
    plan->generation = ++publishedGen_;
    latestPlan_.store(plan.get(), std::memory_order_release);
    plans_.push_back(std::move(plan));
    collectPlans();
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

void Engine::setLoop(double startSeconds, double endSeconds, bool on) {
    const std::uint64_t seq = loopSeq_.load(std::memory_order_relaxed);
    loopSeq_.store(seq + 1, std::memory_order_relaxed);  // odd: being written
    std::atomic_thread_fence(std::memory_order_release);
    loopStart_.store(time::samplesAt(std::max(0.0, startSeconds), config_.sampleRate),
                     std::memory_order_relaxed);
    loopEnd_.store(time::samplesAt(std::max(0.0, endSeconds), config_.sampleRate),
                   std::memory_order_relaxed);
    loopOn_.store(on, std::memory_order_relaxed);
    loopSeq_.store(seq + 2, std::memory_order_release);
    epoch_.fetch_add(1, std::memory_order_release);  // anticipative: re-render ahead
}

LoopRange Engine::readLoop(std::uint64_t* seqOut) const SUPPLE_RT {
    for (;;) {
        const std::uint64_t seq = loopSeq_.load(std::memory_order_acquire);
        LoopRange l;
        l.start = loopStart_.load(std::memory_order_relaxed);
        l.end = loopEnd_.load(std::memory_order_relaxed);
        l.on = loopOn_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if ((seq & 1) == 0 && loopSeq_.load(std::memory_order_relaxed) == seq) {
            if (seqOut) *seqOut = seq;
            return l;
        }
    }
}

std::int64_t Engine::playedPosition(std::uint64_t epoch, std::int64_t epochStart) const {
    return rtPosEpoch_.load(std::memory_order_acquire) == epoch ? rtPos_.load() : epochStart;
}

std::int64_t Engine::playedStream(std::uint64_t epoch) const {
    return rtPosEpoch_.load(std::memory_order_acquire) == epoch ? rtStream_.load() : 0;
}

TransportState Engine::transport() const {
    TransportState t;
    t.playing = reportedPlaying_.load();
    t.sample = options_.anticipative ? playedPosition(epoch_.load(), epochStart_.load())
                                     : reportedPos_.load();
    t.seconds = static_cast<double>(t.sample) / config_.sampleRate;
    t.loop = readLoop();
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

bool Engine::awaitPlan(double timeoutSeconds) {
    if (!ring_) return true;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    while (stoppedGen_.load(std::memory_order_acquire) < publishedGen_)
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        else
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    return true;
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
                renderedStream_.load(std::memory_order_acquire) - playedStream(e);
            if (ahead >= goal) return true;
        }
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

bool Engine::awaitRendererHeld(double timeoutSeconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    while (!holdSeen_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

void Engine::collectPlans() {
    // The oldest generation an executor may still be using.
    const std::uint64_t rendered = stoppedGen_.load(std::memory_order_acquire);
    const std::uint64_t oldest =
        options_.anticipative ? std::min(rendered, callbackGen_.load(std::memory_order_acquire))
                              : rendered;
    // The newest plan always stays (an executor may be about to adopt it).
    std::erase_if(plans_, [&](const std::unique_ptr<detail::Plan>& p) {
        return p->generation < oldest && p.get() != plans_.back().get();
    });
}

void Engine::service() {
    collectPlans();
    if (recorderOwned_) recorderOwned_->drainMidi();  // (the rings are bounded)
    const std::uint64_t rendered = stoppedGen_.load(std::memory_order_acquire);
    const std::uint64_t live =
        options_.anticipative ? std::min(rendered, callbackGen_.load(std::memory_order_acquire))
                              : rendered;
    if (prunePending_ && latest_ && live >= publishedGen_) {
        // Every executor has adopted the newest plan and stopped what it dropped.
        plugins_.prune(*latest_, live);
        prunePending_ = false;
    }
}

namespace {
// An instance whose start_processing() failed: nobody retries it until a plan
// that uses it is adopted again (its stage passes audio through).
const char kRefused = 0;
}  // namespace

void Engine::adoptShare(std::vector<host::Instance*>& owned,
                        const std::vector<host::Instance*>* next, const void* self) SUPPLE_RT {
    // Stop what the new plan no longer gives this executor, keep the rest
    // (lists are sorted; capacity was reserved and checked in load()).
    std::size_t kept = 0;
    for (host::Instance* in : owned) {
        if (next && std::binary_search(next->begin(), next->end(), in)) {
            owned[kept++] = in;
        } else {
            // Leaving this executor (removed, or moving to the other one):
            // no voice may hang on, e.g. live MIDI notes when monitoring stops.
            in->reset();
            in->stopProcessing();
            in->setOwner(nullptr);
        }
    }
    owned.resize(kept);  // shrinking never allocates
    if (next)
        for (host::Instance* in : *next)
            if (in->owner() == &kRefused) in->setOwner(nullptr);  // retry with a new plan
    startFree(owned, next, self);
}

void Engine::startFree(std::vector<host::Instance*>& owned,
                       const std::vector<host::Instance*>* next, const void* self) SUPPLE_RT {
    if (!next) return;
    for (host::Instance* in : *next) {
        // Ours already, refused, or still owned by the other executor.
        if (in->owner() || in->processing()) continue;
        if (!in->startProcessing()) {
            in->setOwner(&kRefused);
            processErrors_.fetch_add(1);
            continue;
        }
        in->setOwner(self);
        owned.push_back(in);  // within the reserved capacity
    }
}

void Engine::resetOwned(const std::vector<host::Instance*>& owned) SUPPLE_RT {
    for (host::Instance* in : owned) in->reset();
}

void Engine::cycle(const float* const* in, float* const* out, std::uint32_t frames) SUPPLE_RT {
    struct InCycle {  // cycleSeq_ is odd from here until return
        std::atomic<std::uint64_t>& seq;
        explicit InCycle(std::atomic<std::uint64_t>& s) : seq(s) { seq.fetch_add(1); }
        ~InCycle() { seq.fetch_add(1, std::memory_order_release); }
    } inCycle(cycleSeq_);
    if (frames == 0) {  // the backend's final callback: stop every plugin here
        // Anticipative: the renderer stopped its share as it ended; the live
        // share is this thread's.
        for (host::Instance* inst : liveProcessing_) {
            inst->stopProcessing();
            inst->setOwner(nullptr);
        }
        liveProcessing_.clear();
        if (!shutDown_.load()) {
            for (host::Instance* inst : processing_) {
                inst->stopProcessing();
                inst->setOwner(nullptr);
            }
            processing_.clear();
            playing_ = false;
            stoppedOnAudioThread_ = true;
            shutDown_.store(true);
        }
        return;
    }
    if (ring_) {  // anticipative: the renderer has done most of the work
        anticipativeCycle(in, out, frames);
        cycles_.fetch_add(1, std::memory_order_release);
        return;
    }
    detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
    if (plan != plan_) {
        if (plan_ && plan) detail::carryLiveNotes(*plan_, *plan, false);  // old plan still ours
        adoptShare(processing_, plan ? &plan->instances : nullptr, &processing_);
        plan_ = plan;
        if (plan) {
            detail::locate(*plan, pos_);
            stoppedGen_.store(plan->generation, std::memory_order_release);
        }
    }
    if (shutdownRequested_.load(std::memory_order_acquire) && !shutDown_.load()) {
        for (host::Instance* inst : processing_) {
            inst->stopProcessing();
            inst->setOwner(nullptr);
        }
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
            resetOwned(processing_);
        }
    }
    if (const bool want = wantPlaying_.load(std::memory_order_acquire); want != playing_) {
        playing_ = want;
        if (!want) resetOwned(processing_);  // no hanging notes
    }
    if (const std::uint64_t seq = loopSeq_.load(std::memory_order_acquire); seq != loopSeqSeen_)
        loopC_ = readLoop(&loopSeqSeen_);
    if (plan) {
        startFree(processing_, &plan->instances, &processing_);  // any refused, retried
        // For monitored inputs (audio and MIDI).
        plan->deviceIn = in;
        plan->deviceInChannels = deviceInputs_;
        plan->deviceMidi = midiSource_->midiInput();
        plan->deviceMidiOffset = 0;
    }
    if (plan && !playing_ && plan->anyLive && !shutDown_.load() && frames <= plan->block) {
        // Stopped, but a track monitors its input: plugins run, nothing plays.
        if (detail::processBlock(*plan, pos_, frames, false)) {
            const detail::Node& master = plan->nodes.back();
            std::copy_n(master.post.l.begin(), frames, out[0]);
            std::copy_n(master.post.r.begin(), frames, out[1]);
        } else {
            processErrors_.fetch_add(1);
            std::fill_n(out[0], frames, 0.0f);
            std::fill_n(out[1], frames, 0.0f);
        }
        reportedPos_.store(pos_);
        reportedPlaying_.store(false);
        cycles_.fetch_add(1, std::memory_order_release);
        return;
    }
    const bool run = plan && playing_ && !shutDown_.load() && frames <= plan->block;
    bool ok = run;
    std::array<const float*, 64> inAt{};
    for (std::uint32_t done = 0; ok && done < frames;) {
        const std::uint32_t k = loopC_.until(pos_, frames - done);  // split at a loop wrap
        for (std::uint32_t c = 0; c < deviceInputs_ && c < inAt.size(); ++c)
            inAt[c] = in && in[c] ? in[c] + done : nullptr;
        const float* const* ins = in ? inAt.data() : nullptr;
        plan->deviceIn = ins;  // monitored input for this part of the cycle
        plan->deviceMidiOffset = done;
        if (!detail::processBlock(*plan, pos_, k, true, &loopC_)) {
            ok = false;
            break;
        }
        const detail::Node& master = plan->nodes.back();
        std::copy_n(master.post.l.begin(), k, out[0] + done);
        std::copy_n(master.post.r.begin(), k, out[1] + done);
        if (detail::Recorder* r = recorder_.load()) {
            r->capture(ins, deviceInputs_, k, pos_);
            r->captureMidi(plan->deviceMidi, done, k, pos_);
        }
        const std::int64_t next = loopC_.next(pos_, pos_ + k);
        if (next != pos_ + k) detail::wrapTo(*plan, next, -1);
        pos_ = next;
        done += k;
    }
    if (!ok) {
        if (run) processErrors_.fetch_add(1);
        std::fill_n(out[0], frames, 0.0f);
        std::fill_n(out[1], frames, 0.0f);
    }
    reportedPos_.store(pos_);
    reportedPlaying_.store(playing_);
    cycles_.fetch_add(1, std::memory_order_release);
}

// --- anticipation (ADR-0012) -----------------------------------------------------------------

Engine::Pulled Engine::pullAhead(float* const* out, std::uint32_t frames) SUPPLE_RT {
    std::int64_t first = kNothingPlayed;
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
        if (done > 0 && c->start + rtOffset_ != first + done)  // a loop wrap: stop there
            return {first, done, true};
        const std::uint32_t take = std::min(frames - done, c->frames - rtOffset_);
        if (first == kNothingPlayed) first = c->start + rtOffset_;
        std::copy_n(c->l.begin() + rtOffset_, take, out[0] + done);
        std::copy_n(c->r.begin() + rtOffset_, take, out[1] + done);
        done += take;
        rtOffset_ += take;
        rtPos_.store(c->start + rtOffset_, std::memory_order_relaxed);
        rtStream_.store(c->stream + rtOffset_, std::memory_order_relaxed);
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
    return {first, done, false};
}

// --- recording (ADR-0014) --------------------------------------------------------------------

Status Engine::startRecording(std::vector<RecordTarget> targets) {
    if (!backend_) return Error("engine.stopped", "start the engine first");
    if (recorderOwned_) return Error("record.running", "already recording");
    if (targets.empty())
        return Error("record.nothing_armed", "no track is armed with an input", {},
                     "track.set {armed: true, record_input: [1, 2]}");
    const BackendStatus b = backend_->status();
    const std::int64_t latency =
        static_cast<std::int64_t>(outputLatency_) + b.outputLatency + b.inputLatency;
    auto rec = std::make_unique<detail::Recorder>(std::move(targets), config_.sampleRate, latency);
    SUPPLE_TRY(rec->open());
    recorderOwned_ = std::move(rec);
    recorder_.store(recorderOwned_.get(), std::memory_order_release);
    return okStatus();
}

Result<std::vector<RecordedTake>> Engine::stopRecording() {
    if (!recorderOwned_) return Error("record.stopped", "not recording");
    recorder_.store(nullptr);
    // A callback that entered before the store may still hold the recorder;
    // one that enters after it sees null. (seq_cst on both sides.)
    if (const std::uint64_t seq = cycleSeq_.load(); seq & 1) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (cycleSeq_.load(std::memory_order_acquire) == seq &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        if (cycleSeq_.load(std::memory_order_acquire) == seq)  // never free what it may use
            return Error("record.busy",
                         "the audio callback has not returned for 2 s; the recording is kept", {},
                         "stop again, or stop the engine (the files stay recoverable)");
    }
    auto takes = recorderOwned_->finish(readLoop());
    recorderOwned_.reset();
    return takes;
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
        detail::locateClass(*plan_, pos_, false);
        resetOwned(processing_);
    }
    epochStartLocal_ = pos_;
    epochStart_.store(pos_);
    streamLocal_ = 0;
    renderedStream_.store(0, std::memory_order_release);
    loopR_ = readLoop();  // a loop change bumps the epoch: this is the new one
    epochDone_.store(epoch, std::memory_order_release);
}

void Engine::rendererLoop(std::uint64_t seen) {
    const std::uint32_t block = options_.renderBlock;
    const auto nap = std::chrono::duration<double>(0.25 * block / config_.sampleRate);
    const void* self = &processing_;
    while (!rendererQuit_.load(std::memory_order_acquire)) {
        detail::Plan* plan = latestPlan_.load(std::memory_order_acquire);
        if (plan != plan_) {  // edits take effect here: after what is already rendered
            adoptShare(processing_, plan ? &plan->rendererInstances : nullptr, self);
            plan_ = plan;
            if (plan) {
                detail::locateClass(*plan, pos_, false);
                plan->handAt = streamLocal_;  // where the callback follows (ADR-0014)
                plan->handEpoch = seen;
                stoppedGen_.store(plan->generation, std::memory_order_release);
                handPlan_.store(plan, std::memory_order_release);
            }
        } else if (plan) {
            startFree(processing_, &plan->rendererInstances, self);  // let go by the callback
        }
        if (const std::uint64_t e = epoch_.load(std::memory_order_acquire); e != seen) {
            relocate(seen, e);
            seen = e;
        }
        if (const bool want = wantPlaying_.load(std::memory_order_acquire); want != playing_) {
            playing_ = want;
            if (!want) resetOwned(processing_);  // no hanging notes
            reportedPlaying_.store(playing_);
        }
        const std::int64_t ahead = streamLocal_ - playedStream(seen);
        const bool held = hold_.load(std::memory_order_acquire);
        if (held) holdSeen_.store(true, std::memory_order_release);
        bool room = playing_ && plan_ && !held && ahead < aheadFrames_;
        if (room && plan_->split) {
            room = !plan_->frontier.empty();  // nothing to hand over: all is live
            for (const auto& r : plan_->frontierRings) room = room && r->slot() != nullptr;
        } else if (room) {
            room = ring_->slot() != nullptr;
        }
        if (!room) {
            std::this_thread::sleep_for(playing_ ? nap : std::chrono::duration<double>(0.001));
            continue;
        }
        const std::uint32_t n = loopR_.until(pos_, block);  // no chunk crosses a loop wrap
        const bool ok =
            detail::processRendererBlock(workers_.get(), *plan_, pos_, n, self, &loopR_);
        if (!ok) processErrors_.fetch_add(1);
        auto fill = [&](detail::Chunk* c, const detail::Stereo& from) {
            if (ok) {
                std::copy_n(from.l.begin(), n, c->l.begin());
                std::copy_n(from.r.begin(), n, c->r.begin());
            } else {
                std::fill(c->l.begin(), c->l.end(), 0.0f);
                std::fill(c->r.begin(), c->r.end(), 0.0f);
            }
            c->epoch = seen;
            c->start = pos_;
            c->stream = streamLocal_;
            c->frames = n;
        };
        if (plan_->split) {  // the frontier taps, for the callback's live nodes
            for (std::size_t j = 0; j < plan_->frontier.size(); ++j) {
                const detail::Edge& e = plan_->edges[plan_->frontier[j]];
                const detail::Node& src = plan_->nodes[e.from];
                fill(plan_->frontierRings[j]->slot(), e.tap == model::Tap::PreFx      ? src.preFx
                                                      : e.tap == model::Tap::PreFader ? src.preFader
                                                                                      : src.post);
                plan_->frontierRings[j]->publish();
            }
        } else {
            fill(ring_->slot(), plan_->nodes.back().post);
            ring_->publish();
        }
        const std::int64_t next = loopR_.next(pos_, pos_ + n);
        if (next != pos_ + n) detail::wrapTo(*plan_, next, 0);
        pos_ = next;
        streamLocal_ += n;
        renderedStream_.store(streamLocal_, std::memory_order_release);
    }
    // The renderer is its plugins' audio thread: stop them here.
    for (host::Instance* in : processing_) {
        in->stopProcessing();
        in->setOwner(nullptr);
    }
    processing_.clear();
    playing_ = false;
    reportedPlaying_.store(false);
    stoppedOnAudioThread_ = true;
    shutDown_.store(true);
}

// --- the live region (ADR-0014) -----------------------------------------------------------

void Engine::adoptLive(detail::Plan* next) SUPPLE_RT {
    if (livePlan_ && next && next->split) detail::carryLiveNotes(*livePlan_, *next, true);
    adoptShare(liveProcessing_, next ? &next->liveInstances : nullptr, &liveProcessing_);
    const bool wasLive = livePlan_ && livePlan_->split;
    livePlan_ = next;
    if (!next) return;
    if (next->split) {
        if (wasLive && liveReady_)
            detail::locateClass(*next, livePos_, true);  // continue at the switch position
        else
            liveReady_ = false;  // entering the live mode: liveRender takes the position
    }
    callbackGen_.store(next->generation, std::memory_order_release);
}

void Engine::anticipativeCycle(const float* const* in, float* const* out,
                               std::uint32_t frames) SUPPLE_RT {
    const std::uint64_t e = epoch_.load(std::memory_order_acquire);
    if (e != rtEpoch_) {
        rtEpoch_ = e;
        rtPrimed_ = false;
        liveReady_ = false;
        loopC_ = readLoop();  // the renderer takes the loop with the epoch too
        rtEpochSeen_.store(e, std::memory_order_release);
    }
    const bool playing = wantPlaying_.load(std::memory_order_acquire);
    std::array<const float*, 64> inAt{};
    const std::uint32_t inputs = deviceInputs_;
    for (std::uint32_t done = 0; done < frames;) {
        std::uint32_t k = frames - done;
        // Follow the renderer's plan where it took effect (split the cycle there).
        if (detail::Plan* h = handPlan_.load(std::memory_order_acquire); h != livePlan_) {
            const std::int64_t here =  // stream positions: they only grow (ADR-0015)
                livePlan_ && livePlan_->split && liveReady_ ? liveStream_ : playedStream(e);
            if (!h || h->handEpoch != e || !playing || here >= h->handAt)
                adoptLive(h);
            else if (h->handAt - here < k)
                k = static_cast<std::uint32_t>(h->handAt - here);
        }
        for (std::uint32_t c = 0; c < inputs; ++c) inAt[c] = in && in[c] ? in[c] + done : nullptr;
        float* outAt[2] = {out[0] + done, out[1] + done};
        const float* const* ins = in ? inAt.data() : nullptr;
        if (livePlan_ && livePlan_->split) {
            k = liveRender(ins, outAt, k, done);
        } else {
            const Pulled played = pullAhead(outAt, k);
            if (played.wrapped) k = played.frames;  // the rest continues at the loop start
            // Only input that went with played audio is recorded; what came
            // during a dropout had no timeline position and counts as lost.
            if (detail::Recorder* r = recorder_.load()) {
                if (played.first != kNothingPlayed) {
                    r->capture(ins, inputs, played.frames, played.first);
                    r->captureMidi(midiSource_->midiInput(), done, played.frames, played.first);
                    r->lose(k - played.frames);
                } else if (playing) {
                    r->lose(k);
                }
            }
        }
        done += k;
    }
}

std::uint32_t Engine::liveRender(const float* const* in, float* const* out, std::uint32_t k,
                                 std::uint32_t offset) SUPPLE_RT {
    detail::Plan& p = *livePlan_;
    const void* self = &liveProcessing_;
    if (!liveReady_) {
        if (epochDone_.load(std::memory_order_acquire) < rtEpoch_) {  // not placed yet
            std::fill_n(out[0], k, 0.0f);
            std::fill_n(out[1], k, 0.0f);
            return k;
        }
        livePos_ = playedPosition(rtEpoch_, epochStart_.load());
        liveStream_ = playedStream(rtEpoch_);
        detail::locateClass(p, livePos_, true);
        resetOwned(liveProcessing_);
        liveReady_ = true;
    }
    startFree(liveProcessing_, &p.liveInstances, self);  // let go by the renderer
    // The frontier: what the renderer made for this position. Before its
    // first chunk (pre-roll) time holds still; the monitored input is heard.
    bool playing = wantPlaying_.load(std::memory_order_acquire);
    if (playing) k = loopC_.until(livePos_, k);  // stop at a loop wrap
    std::uint32_t found = k;
    for (std::size_t j = 0; j < p.frontier.size(); ++j) {
        detail::Edge& e = p.edges[p.frontier[j]];
        const std::uint32_t got =
            playing ? p.frontierRings[j]->pullAt(rtEpoch_, liveStream_, e.frontierBuf.l.data(),
                                                 e.frontierBuf.r.data(), k)
                    : 0;
        if (!playing) e.frontierBuf.clear(k);
        found = std::min(found, got);
    }
    if (playing && !p.frontier.empty()) {
        if (found > 0) {
            rtPrimed_ = true;
        } else if (!rtPrimed_) {
            playing = false;  // pre-roll
        }
        if (rtPrimed_ && found < k) misses_.fetch_add(1, std::memory_order_relaxed);
    }
    p.deviceIn = in;
    p.deviceInChannels = deviceInputs_;
    p.deviceMidi = midiSource_->midiInput();
    p.deviceMidiOffset = offset;
    host::Transport t = detail::transportAt(p, livePos_, &loopC_);
    t.playing = playing;
    bool ok = true;
    for (std::size_t i : p.liveNodes) ok = detail::processNode(p, i, livePos_, k, t, self) && ok;
    if (!ok) processErrors_.fetch_add(1);
    const detail::Node& master = p.nodes.back();
    std::copy_n(master.post.l.begin(), k, out[0]);
    std::copy_n(master.post.r.begin(), k, out[1]);
    if (playing) {
        if (detail::Recorder* r = recorder_.load()) {
            r->capture(in, deviceInputs_, k, livePos_);
            r->captureMidi(p.deviceMidi, offset, k, livePos_);
        }
        const std::int64_t next = loopC_.next(livePos_, livePos_ + k);
        if (next != livePos_ + k) detail::wrapTo(p, next, 1);
        livePos_ = next;
        liveStream_ += k;
        rtPos_.store(livePos_, std::memory_order_relaxed);
        rtStream_.store(liveStream_, std::memory_order_relaxed);
        rtPosEpoch_.store(rtEpoch_, std::memory_order_release);
    }
    return k;
}

}  // namespace supple::engine
