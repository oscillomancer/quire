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
    for (std::size_t port = 0; port < names.size() && port < queues_.size(); ++port) {
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
            period_[count_++] = event;
        }
    }
    // Stable insertion sort, bounded by kMaxMidiInputEvents, allocation-free.
    for (std::size_t i = 1; i < count_; ++i)
        for (std::size_t k = i; k && period_[k].frame < period_[k - 1].frame; --k)
            std::swap(period_[k], period_[k - 1]);
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
