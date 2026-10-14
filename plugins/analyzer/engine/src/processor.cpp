// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "analyzer/processor.h"

#include <algorithm>

namespace supple::analyzer {
void Processor::prepare(double sampleRate, std::uint32_t maxFrames, int channels) {
    (void)maxFrames;
    sampleRate_ = std::clamp(sampleRate, 22050.0, 384000.0);
    channels_ = std::clamp(channels, 1, 2);
    std::size_t capacity = 1;
    while (static_cast<double>(capacity * kPacketFrames) < sampleRate_ * 2) capacity *= 2;
    packets_.resize(capacity);
    mask_ = capacity - 1;
    write_ = 0;
    read_ = 0;
    dropped_ = 0;
    pending_ = {};
    ++epoch_;
    consumerEpoch_ = epoch_;
    consumerDropped_ = 0;
    consumerWindow_ = static_cast<int>(values_[kWindow]);
    consumerHeld_ = values_[kHold] != 0;
    meter_.prepare(sampleRate_, channels_, windowSeconds(values_[kWindow]));
    reportSamples_ = 0;
    publish();
}
void Processor::enqueue() SUPPLE_RT {
    pending_.window = static_cast<int>(values_[kWindow]);
    pending_.held = values_[kHold] != 0;
    pending_.epoch = epoch_;
    pending_.dropped = dropped_.load(std::memory_order_relaxed);
    const auto w = write_.load(std::memory_order_relaxed),
               r = read_.load(std::memory_order_acquire);
    if (w - r < packets_.size()) {
        packets_[w & mask_] = pending_;
        write_.store(w + 1, std::memory_order_release);
    } else
        dropped_.fetch_add(pending_.frames, std::memory_order_relaxed);
    pending_.frames = 0;
}
void Processor::reset() SUPPLE_RT {
    pending_.frames = 0;
    ++epoch_;
    enqueue();
}
void Processor::setParam(std::size_t index, double physical) SUPPLE_RT {
    if (index > kHold) return;
    const double value = params::clampPhysical(table()[index], physical);
    if (index == kReset) {
        if (value != 0) reset();
        values_[kReset] = 0;
        return;
    }
    if (values_[index] == value) return;
    if (pending_.frames) enqueue();
    values_[index] = value;
    if (index == kWindow) ++epoch_;
    enqueue();
}
void Processor::drain() {
    auto r = read_.load(std::memory_order_relaxed);
    const auto end = write_.load(std::memory_order_acquire);
    while (r < end) {
        const auto& packet = packets_[r & mask_];
        bool changed = false;
        if (packet.epoch != consumerEpoch_ || packet.dropped != consumerDropped_) {
            if (packet.window != consumerWindow_)
                meter_.prepare(sampleRate_, channels_, windowSeconds(packet.window));
            else
                meter_.reset();
            consumerWindow_ = packet.window;
            consumerEpoch_ = packet.epoch;
            consumerDropped_ = packet.dropped;
            reportSamples_ = 0;
            changed = true;
        }
        if (packet.held != consumerHeld_) {
            consumerHeld_ = packet.held;
            changed = true;
        }
        if (!consumerHeld_) {
            const float* input[]{packet.left.data(), packet.right.data()};
            meter_.process(
                std::span<const float* const>(input, static_cast<std::size_t>(channels_)),
                static_cast<int>(packet.frames));
            reportSamples_ += packet.frames;
        }
        if (changed ||
            reportSamples_ >= static_cast<std::uint64_t>(std::llround(sampleRate_ * 0.05))) {
            publish();
            reportSamples_ %= static_cast<std::uint64_t>(std::llround(sampleRate_ * 0.05));
        }
        ++r;
        read_.store(r, std::memory_order_release);
    }
}
void Processor::publish() {
    const auto seq = snapshotSeq_.load(std::memory_order_relaxed);
    snapshotSeq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const auto fields =
        std::bit_cast<std::array<double, kMeasurementFields>>(meter_.measurements());
    for (std::size_t i = 0; i < fields.size(); ++i)
        snapshot_[i].store(fields[i], std::memory_order_relaxed);
    snapshotElapsed_.store(meter_.elapsedSeconds(), std::memory_order_relaxed);
    snapshotWindow_.store(consumerWindow_, std::memory_order_relaxed);
    snapshotHeld_.store(consumerHeld_, std::memory_order_relaxed);
    snapshotDropped_.store(consumerDropped_, std::memory_order_relaxed);
    snapshotSeq_.store(seq + 2, std::memory_order_release);
}
bool Processor::readFrame(Frame& out) const SUPPLE_RT {
    const auto seq = snapshotSeq_.load(std::memory_order_acquire);
    if (seq & 1) return false;
    std::array<double, kMeasurementFields> fields{};
    for (std::size_t i = 0; i < fields.size(); ++i)
        fields[i] = snapshot_[i].load(std::memory_order_relaxed);
    Frame frame;
    frame.measurements = std::bit_cast<analysis::LiveMeasurements>(fields);
    frame.elapsedSeconds = snapshotElapsed_.load(std::memory_order_relaxed);
    frame.window = snapshotWindow_.load(std::memory_order_relaxed);
    frame.held = snapshotHeld_.load(std::memory_order_relaxed);
    frame.droppedFrames = snapshotDropped_.load(std::memory_order_relaxed);
    frame.sampleRate = sampleRate_;
    frame.channels = channels_;
    frame.generation = seq;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (snapshotSeq_.load(std::memory_order_relaxed) != seq) return false;
    out = frame;
    return true;
}
json::Value telemetryJson(const Frame& f) {
    auto out = analysis::toJson(f.measurements, f.sampleRate, f.channels);
    out["live"]["window"] = params::toJson(table()[kWindow], f.window);
    out["live"]["hold"] = f.held;
    out["live"]["dropped_frames"] = static_cast<double>(f.droppedFrames);
    return out;
}
}  // namespace supple::analyzer
