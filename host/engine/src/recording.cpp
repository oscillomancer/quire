#include "recording.h"

#include <algorithm>
#include <chrono>

namespace supple::engine::detail {

void FrameRing::init(std::uint32_t channels, std::size_t frames) {
    channels_ = channels;
    capacity_ = frames;
    buf_.assign(frames * channels, 0.0f);
    write_ = 0;
    read_ = 0;
}

std::size_t FrameRing::write(const float* const* in, std::uint32_t inChannels,
                             std::span<const int> select, std::uint32_t frames) SUPPLE_RT {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t room = capacity_ - (w - read_.load(std::memory_order_acquire));
    const std::size_t n = std::min<std::size_t>(frames, room);
    for (std::size_t i = 0; i < n; ++i) {
        float* dst = buf_.data() + ((w + i) % capacity_) * channels_;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const int src = c < select.size() ? select[c] : -1;
            dst[c] = in && src >= 0 && static_cast<std::uint32_t>(src) < inChannels && in[src]
                         ? in[src][i]
                         : 0.0f;
        }
    }
    write_.store(w + n, std::memory_order_release);
    return n;
}

std::size_t FrameRing::read(float* out, std::size_t maxFrames) {
    const std::size_t r = read_.load(std::memory_order_relaxed);
    const std::size_t n = std::min(maxFrames, write_.load(std::memory_order_acquire) - r);
    for (std::size_t i = 0; i < n; ++i)
        std::copy_n(buf_.data() + ((r + i) % capacity_) * channels_, channels_,
                    out + i * channels_);
    read_.store(r + n, std::memory_order_release);
    return n;
}

Recorder::Recorder(std::vector<RecordTarget> targets, double rate, std::int64_t latency)
    : rate_(rate), latency_(latency) {
    for (RecordTarget& t : targets) {
        auto track = std::make_unique<Track>();
        track->target = std::move(t);
        track->ring.init(
            static_cast<std::uint32_t>(std::max<std::size_t>(1, track->target.inputs.size())),
            static_cast<std::size_t>(rate * 10.0));  // ten seconds
        tracks_.push_back(std::move(track));
    }
}

Recorder::~Recorder() {
    stop_ = true;
    if (writer_.joinable()) writer_.join();
}

Status Recorder::open() {
    for (auto& t : tracks_) {
        std::error_code ec;
        std::filesystem::create_directories(t->target.file.parent_path(), ec);
        SUPPLE_TRY(t->writer.open(t->target.file, rate_, t->ring.channels()));
    }
    writer_ = std::thread([this] { writerLoop(); });
    return okStatus();
}

void Recorder::capture(const float* const* in, std::uint32_t inChannels, std::uint32_t frames,
                       std::int64_t pos) SUPPLE_RT {
    if (!started_.load(std::memory_order_relaxed)) {
        // What the performer heard at `pos` reaches us a round trip later.
        start_.store(pos - latency_, std::memory_order_relaxed);
        started_.store(true, std::memory_order_release);
    }
    for (auto& t : tracks_) {
        const std::size_t n = t->ring.write(in, inChannels, t->target.inputs, frames);
        if (n < frames) t->overflow.fetch_add(frames - n, std::memory_order_relaxed);
    }
}

void Recorder::drain(Track& t, std::vector<float>& scratch) {
    const std::size_t chunk = scratch.size() / t.ring.channels();
    for (;;) {
        const std::size_t n = t.ring.read(scratch.data(), chunk);
        if (n == 0) return;
        if (writeError_) {
            auto st = t.writer.append(scratch.data(), n);
            if (!st) writeError_ = std::move(st);
        }
    }
}

void Recorder::writerLoop() {
    std::vector<float> scratch(8192 * 16);
    auto lastHeader = std::chrono::steady_clock::now();
    while (!stop_.load(std::memory_order_acquire)) {
        for (auto& t : tracks_) drain(*t, scratch);
        if (std::chrono::steady_clock::now() - lastHeader > std::chrono::seconds(5)) {
            for (auto& t : tracks_) (void)t->writer.updateHeader();  // a crash keeps this much
            lastHeader = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (auto& t : tracks_) drain(*t, scratch);  // what arrived before the stop
}

std::vector<RecordedTake> Recorder::finish() {
    stop_.store(true, std::memory_order_release);
    if (writer_.joinable()) writer_.join();
    std::vector<RecordedTake> out;
    const std::int64_t start =
        started_.load(std::memory_order_acquire) ? start_.load(std::memory_order_relaxed) : 0;
    for (auto& t : tracks_) {
        RecordedTake r;
        r.track = t->target.track;
        r.file = t->target.file;
        r.startSample = start;
        r.frames = static_cast<std::int64_t>(t->writer.frames());
        r.overflowFrames = t->overflow.load();
        (void)t->writer.close();
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace supple::engine::detail
