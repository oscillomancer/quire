// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "midi_input.h"
#include <algorithm>
#include <cmath>
namespace supple::engine::detail {
void MidiInputBuffer::push(std::size_t port, double seconds, std::uint32_t message) SUPPLE_RT {
    const auto status = static_cast<std::uint8_t>(message);
    // Match JACK's channel-message contract; system/SysEx are counted drops.
    if (port >= kMidiInputPorts || status < 0x80 || status >= 0xf0 || !std::isfinite(seconds)) {
        drop();
        return;
    }
    TimedMidi event;
    event.seconds = seconds;
    event.size = (status & 0xe0) == 0xc0 ? 2 : 3;
    for (unsigned i = 0; i < event.size; ++i)
        event.bytes[i] = static_cast<std::uint8_t>(message >> (8 * i));
    if ((event.bytes[1] & 0x80) || (event.size == 3 && (event.bytes[2] & 0x80)) ||
        !queues_[port].push(event))
        drop();
}
void MidiInputBuffer::discard(std::size_t port) {
    TimedMidi event;
    while (queues_[port].pop(event)) {
    }
}
void MidiInputBuffer::collect(double end, double rate, std::uint32_t frames) SUPPLE_RT {
    count_ = next_ = 0;
    std::array<std::size_t, kMidiInputPorts> heads{}, ends{};
    for (std::size_t port = 0; port < names.size() && port < queues_.size(); ++port) {
        const auto begin = count_;
        heads[port] = begin;
        bool monotonic = true;
        const auto available = queues_[port].size();
        for (std::size_t i = 0; i < available; ++i) {
            TimedMidi input;
            if (!queues_[port].pop(input)) break;
            if (!frames || count_ == period_.size()) {
                drop();
                continue;
            }
            // Use relative arithmetic to preserve precision with large host times.
            const double frame = (input.seconds - end) * rate + frames;
            MidiInputEvent event;
            event.frame = frame <= 0            ? 0
                          : frame >= frames - 1 ? frames - 1
                                                : static_cast<std::uint32_t>(std::llround(frame));
            event.port = static_cast<std::uint16_t>(port);
            event.size = input.size;
            std::copy_n(input.bytes, input.size, event.bytes);
            if (count_ > begin && event.frame < block_[count_ - 1].frame) monotonic = false;
            block_[count_++] = event;
        }
        ends[port] = count_;
        // Timestamp order is normally arrival order. A driver can deliver an
        // older timestamp later; repair only that run, bounded by 1024 events.
        if (!monotonic)
            for (std::size_t i = begin + 1; i < count_; ++i)
                for (std::size_t k = i; k > begin && block_[k].frame < block_[k - 1].frame; --k)
                    std::swap(block_[k], block_[k - 1]);
    }
    // Merge the per-port runs in O(n * k), k <= 16. Scanning ports in index
    // order and replacing only for an earlier frame preserves equal-frame ties.
    for (std::size_t i = 0; i < count_; ++i) {
        std::size_t best = kMidiInputPorts;
        for (std::size_t port = 0; port < kMidiInputPorts; ++port)
            if (heads[port] < ends[port] &&
                (best == kMidiInputPorts || block_[heads[port]].frame < block_[heads[best]].frame))
                best = port;
        period_[i] = block_[heads[best]++];
    }
}
std::span<const MidiInputEvent> MidiInputBuffer::chunk(std::uint32_t offset,
                                                       std::uint32_t frames) SUPPLE_RT {
    std::size_t count = 0;
    while (next_ < count_ && period_[next_].frame < offset + frames) {
        auto event = period_[next_++];
        event.frame -= offset;
        block_[count++] = event;
    }
    return {block_.data(), count};
}
double winMidiSeconds(std::uint32_t milliseconds, double anchor, double now) SUPPLE_RT {
    constexpr double wrap = 4294967.296;
    const double elapsed = static_cast<double>(milliseconds) * .001;
    const double epochs = std::max(0.0, std::round((now - anchor - elapsed) / wrap));
    return anchor + elapsed + epochs * wrap;
}
#if !defined(__APPLE__) && !defined(_WIN32)
std::unique_ptr<MidiSource> nativeMidiSource() { return {}; }
#endif
}  // namespace supple::engine::detail
