#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include "analyzer/params.h"
#include "supple/analysis/live.h"

namespace supple::analyzer {
struct Frame {
    analysis::LiveMeasurements measurements{};
    double sampleRate = 48000, elapsedSeconds = 0;
    int channels = 2, window = 0;
    bool held = false;
    std::uint64_t droppedFrames = 0, generation = 0;
};

// One audio producer and one measurement consumer. prepare() requires both
// stopped. drain() belongs to the consumer; readFrame() is a wait-free snapshot
// usable from any thread. The plugin owns and joins the consumer worker.
class Processor {
    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

  public:
    void prepare(double sampleRate, std::uint32_t maxFrames, int channels = 2);
    void reset() SUPPLE_RT;
    void setParam(std::size_t index, double physical) SUPPLE_RT;
    std::uint32_t latency() const { return 0; }
    void drain();
    bool readFrame(Frame& out) const SUPPLE_RT;
    std::uint64_t droppedFrames() const { return dropped_.load(std::memory_order_relaxed); }

    template <typename T>
    void process(const T* const* in, T* const* out, std::uint32_t frames) SUPPLE_RT {
        dsp::DenormalGuard guard;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const T l = in[0][i], r = channels_ == 1 ? l : in[1][i];
            out[0][i] = l;
            if (channels_ == 2) out[1][i] = r;
            const auto safe = [](T x) {
                const auto f = static_cast<float>(x);
                return std::isfinite(f) ? f : 0.0f;
            };
            pending_.left[pending_.frames] = safe(l);
            pending_.right[pending_.frames] = safe(r);
            if (++pending_.frames == kPacketFrames) enqueue();
        }
    }
    template <typename T>
    void process(T* const* io, int frames, bool = false) SUPPLE_RT {
        if (!io || frames <= 0) return;
        const T* input[]{io[0], channels_ == 1 ? io[0] : io[1]};
        process(input, io, static_cast<std::uint32_t>(frames));
    }

  private:
    static constexpr std::uint32_t kPacketFrames = 256;
    struct Packet {
        std::array<float, kPacketFrames> left{}, right{};
        std::uint32_t frames = 0;
        int window = 0;
        bool held = false;
        std::uint64_t epoch = 0, dropped = 0;
    };
    std::vector<Packet> packets_;
    std::size_t mask_ = 0;
    std::atomic<std::size_t> write_{0}, read_{0};
    std::atomic<std::uint64_t> dropped_{0};
    Packet pending_{};
    Values values_ = defaults();
    double sampleRate_ = 48000;
    int channels_ = 2;
    std::uint64_t epoch_ = 1, consumerEpoch_ = 0, consumerDropped_ = 0;
    int consumerWindow_ = 0;
    bool consumerHeld_ = false;
    analysis::LiveMeter meter_;
    std::uint64_t reportSamples_ = 0;
    std::atomic<std::uint64_t> snapshotSeq_{0};
    static constexpr std::size_t kMeasurementFields = 44;
    std::array<std::atomic<double>, kMeasurementFields> snapshot_{};
    std::atomic<double> snapshotElapsed_{0};
    std::atomic<int> snapshotWindow_{0};
    std::atomic<bool> snapshotHeld_{false};
    std::atomic<std::uint64_t> snapshotDropped_{0};
    void enqueue() SUPPLE_RT;
    void publish();
};
json::Value telemetryJson(const Frame& frame);
}  // namespace supple::analyzer
