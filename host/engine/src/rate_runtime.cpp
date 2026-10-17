// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/engine/realtime.h"

#include <bit>
#include <exception>

#include "plan.h"

namespace supple::engine {
namespace {
using Clock = detail::PlayrateClock;
using Timeline = detail::PlayrateTimeline;
static_assert(sizeof(Clock::State) == sizeof(std::array<std::uint64_t, 6>));
}  // namespace

std::array<std::uint64_t, 13> Engine::readRateClock() const {
    std::array<std::uint64_t, 13> words{};
    for (;;) {
        const auto seq = rateClockSequence_.load(std::memory_order_acquire);
        if (seq & 1) {
            std::this_thread::yield();
            continue;
        }
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = rateClockWords_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq == rateClockSequence_.load(std::memory_order_relaxed)) return words;
    }
}

void Engine::publishRateClock(detail::Plan& p, std::int64_t device, std::int64_t stream,
                              std::uint64_t epoch, bool live) SUPPLE_RT {
    Clock::State clock;
    auto looping = loopC_.active() && rateEpochStart_ < double(loopC_.end);
    if (p.rateEnabled) {
        const auto snapshot = p.timelines[live ? 1 : 0]->snapshot(device);
        clock = snapshot.clock;
        looping = snapshot.looping;
    } else
        clock.timeline = rateEpochStart_ + double(stream);
    const auto packed = std::bit_cast<std::array<std::uint64_t, 6>>(clock);
    std::array<std::uint64_t, 13> words{};
    std::copy(packed.begin(), packed.end(), words.begin());
    words[6] = std::bit_cast<std::uint64_t>(device);
    words[7] = std::bit_cast<std::uint64_t>(stream);
    words[8] = epoch;
    words[9] = p.generation;
    words[10] = std::bit_cast<std::uint64_t>(double(loopC_.start));
    words[11] = std::bit_cast<std::uint64_t>(double(loopC_.end));
    words[12] = looping ? 1 : 0;
    if (rateDeferredWake_.load(std::memory_order_acquire) &&
        (epoch != rateSafeEpoch_.load(std::memory_order_acquire) ||
         stream + (options_.anticipative
                       ? aheadFrames_
                       : static_cast<std::int64_t>(config_.sampleRate * options_.renderAheadMs /
                                                   1000.)) >=
             rateSafeStream_.load(std::memory_order_acquire))) {
        if (rateDeferredWake_.exchange(false)) notifyMain();
    }
    const auto seq = rateClockSequence_.load(std::memory_order_relaxed);
    rateClockSequence_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < words.size(); ++i)
        rateClockWords_[i].store(words[i], std::memory_order_relaxed);
    rateClockSequence_.store(seq + 2, std::memory_order_release);
}

Status Engine::prepareRatePlan(detail::Plan& p) {
    const auto transport = transportSnapshot();
    p.handEpoch = transport.epoch + (rateSeekTarget_ ? 1 : 0);
    p.rateSeekSequence = rateSeekTarget_ ? transport.seekSeq + 1 : 0;
    const auto words = readRateClock();
    Timeline::Snapshot seed;
    std::array<std::uint64_t, 6> packed{};
    std::copy_n(words.begin(), 6, packed.begin());
    seed.clock = words[9] == 0 ? Clock::State{} : std::bit_cast<Clock::State>(packed);
    seed.device = std::bit_cast<std::int64_t>(words[6]);
    seed.loopStart = std::bit_cast<double>(words[10]);
    seed.loopEnd = std::bit_cast<double>(words[11]);
    seed.looping = words[12] != 0;
    std::int64_t baseStream = std::bit_cast<std::int64_t>(words[7]);
    detail::Plan* previous = nullptr;
    for (const auto& candidate : plans_)
        if (candidate->generation == words[9]) previous = candidate.get();
    // Prepared trajectories form an ordered control history even before the
    // listener reaches them. Replacing pending plans would debounce a drag.
    if (ratePreparedTail_ && ratePreparedTail_->handEpoch == transport.epoch &&
        ratePreparedTail_->generation > words[9]) {
        previous = ratePreparedTail_;
        seed = previous->rateControl[0];
        baseStream = previous->handAt;
    }
    p.rateFromGeneration = previous ? previous->generation : 0;
    const bool playing = previous && wantPlaying_.load(std::memory_order_acquire);
    const LoopRange loop = rateLoopTarget_.value_or(transport.loop);
    const auto heard = playedStream(transport.epoch);
    std::int64_t handStream = heard;
    std::int64_t handDevice = seed.device;
    if (rateSeekTarget_) {
        handStream = 0;
        handDevice = static_cast<std::int64_t>(std::llround(*rateSeekTarget_));
    } else if (playing) {
        const auto lead =
            options_.anticipative
                ? aheadFrames_
                : static_cast<std::int64_t>(config_.sampleRate * options_.renderAheadMs / 1000.);
        handStream = std::max(heard + std::max<std::int64_t>(lead, 64) + 64, baseStream + 64);
        handStream = (handStream + 63) / 64 * 64;
        handDevice = seed.device + handStream - baseStream;
    }
    p.handAt = handStream;
    Status result;
    try {
        if (rateSeekTarget_ || !playing) {
            const double timeline =
                rateSeekTarget_ ? double(*rateSeekTarget_) : seed.clock.timeline;
            result = detail::prepareRateSources(p, handDevice, timeline, &loop);
            p.rateOrigin = handDevice - handStream;
        } else {
            const auto clockClass = options_.anticipative ? 1u : 0u;
            const auto points = previous && previous->rateEnabled
                                    ? previous->timelines[clockClass]->points()
                                    : std::vector<detail::RatePoint>{};
            Timeline old(p.timelines[0]->capacity(), seed.clock.target, points);
            old.seed(seed);
            if (previous && previous->rateEnabled)
                old.inheritControls(*previous->timelines[clockClass]);
            if (previous && previous->rateEnabled) {
                const auto past = seed.device - p.rateDeviceMargin - 64;
                if (!old.copyPast(*previous->timelines[clockClass], past, seed.device)) {
                    result = Error("playrate.history",
                                   "the changed graph needs unavailable clock history", {},
                                   "prepare a fresh seek at the consumed position");
                    return result;
                }
            }
            for (int attempt = 0; attempt < 3; ++attempt) {
                const auto began = std::chrono::steady_clock::now();
                old.ensure(handDevice + p.rateDeviceMargin +
                           static_cast<std::int64_t>(p.rate * .15) + p.block + 128);
                // The legacy unity executor wraps its device coordinate;
                // the rate executor keeps it monotonic from adoption on.
                if (attempt == 0 && (!previous || !previous->rateEnabled)) {
                    const auto wrapped =
                        static_cast<std::int64_t>(std::llround(old.position(handDevice)));
                    const auto offset = wrapped - handDevice;
                    handDevice = wrapped;
                    old.seed(seed, offset);
                    old.ensure(handDevice + p.rateDeviceMargin +
                               static_cast<std::int64_t>(p.rate * .15) + p.block + 128);
                }
                p.rateOrigin = handDevice - handStream;
                result = detail::configureRateTransition(p, previous, old, handDevice);
                if (!result || !backend_->ownsThread() ||
                    playedStream(transport.epoch) + 64 < handStream)
                    break;
                // Priming must finish before its scheduled boundary. A busy
                // machine can need more than the ordinary render horizon;
                // predict farther ahead and prepare again while old audio runs.
                const auto elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                const auto lead = static_cast<std::int64_t>(std::ceil(elapsed * p.rate * 2)) + 128;
                const auto oldHand = handStream;
                handStream = (playedStream(transport.epoch) + lead + 63) / 64 * 64;
                handDevice += handStream - oldHand;
                p.handAt = handStream;
                if (attempt == 2)
                    result = Error("playrate.prepare_late",
                                   "source preparation could not reach a future boundary", {},
                                   "prepare a fresh seek at the consumed position");
            }
        }
        p.rateHandDevice = handDevice;
    } catch (const std::exception& error) {
        return Error("playrate.prepare", error.what(), {},
                     "reduce source/FX count and retry the transport request");
    }
    return result;
}

}  // namespace supple::engine
