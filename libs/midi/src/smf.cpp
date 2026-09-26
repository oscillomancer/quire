#include <supple/midi/midi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace supple::midi {
namespace {

using Bytes = std::vector<std::uint8_t>;

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at = 0;

    bool take(std::size_t count, std::span<const std::uint8_t>& out) {
        if (count > bytes.size() - at) return false;
        out = bytes.subspan(at, count);
        at += count;
        return true;
    }
    bool byte(std::uint8_t& out) {
        std::span<const std::uint8_t> part;
        if (!take(1, part)) return false;
        out = part[0];
        return true;
    }
    bool be16(std::uint16_t& out) {
        std::span<const std::uint8_t> part;
        if (!take(2, part)) return false;
        out = static_cast<std::uint16_t>((part[0] << 8) | part[1]);
        return true;
    }
    bool be32(std::uint32_t& out) {
        std::span<const std::uint8_t> part;
        if (!take(4, part)) return false;
        out = (std::uint32_t(part[0]) << 24) | (std::uint32_t(part[1]) << 16) |
              (std::uint32_t(part[2]) << 8) | part[3];
        return true;
    }
    bool vlq(std::uint32_t& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t b = 0;
            if (!byte(b)) return false;
            out = (out << 7) | (b & 0x7f);
            if ((b & 0x80) == 0) return true;
        }
        return false;
    }
};

Error badSmf(const char* reason) {
    return Error{
        "midi.smf_invalid", reason, {}, "Use a format 0 or 1 Standard MIDI File with PPQ timing."};
}

bool tagIs(std::span<const std::uint8_t> bytes, const char* expected) {
    return bytes.size() == 4 && std::equal(bytes.begin(), bytes.end(), expected);
}

void be16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void be32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 24));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void vlq(Bytes& b, std::uint32_t v) {
    std::array<std::uint8_t, 4> parts{};
    int n = 0;
    do {
        parts[static_cast<std::size_t>(n++)] = static_cast<std::uint8_t>(v & 0x7f);
        v >>= 7;
    } while (v != 0 && n < 4);
    while (n > 1)
        b.push_back(static_cast<std::uint8_t>(parts[static_cast<std::size_t>(--n)] | 0x80));
    b.push_back(parts[0]);
}

void emitDelta(Bytes& track, std::int64_t delta) {
    while (delta > 0x0fffffff) {
        vlq(track, 0x0fffffff);
        track.insert(track.end(), {0xff, 0x01, 0x00});
        delta -= 0x0fffffff;
    }
    vlq(track, static_cast<std::uint32_t>(delta));
}

struct Event {
    std::int64_t tick;
    int order;
    Bytes data;
};

struct NoteAction {
    std::int64_t tick;
    int track;
    std::size_t order;
    std::uint8_t channel;
    std::uint8_t pitch;
    std::uint8_t velocity;
    bool on;
};

}  // namespace

Result<Sequence> readSmf(std::span<const std::uint8_t> bytes, SmfInfo* info) {
    Reader file{bytes};
    std::span<const std::uint8_t> tag;
    std::uint32_t headerSize = 0;
    std::uint16_t format = 0, tracks = 0, division = 0;
    if (!file.take(4, tag) || !tagIs(tag, "MThd") || !file.be32(headerSize) || headerSize < 6 ||
        !file.be16(format) || !file.be16(tracks) || !file.be16(division))
        return badSmf("The MIDI header is incomplete.");
    if (format > 1 || tracks == 0 || (format == 0 && tracks != 1) || division == 0 ||
        (division & 0x8000) != 0)
        return badSmf("The MIDI format or time division is unsupported.");
    if (!file.take(headerSize - 6, tag)) return badSmf("The MIDI header is truncated.");
    Sequence out;
    out.ppq = division;
    SmfInfo metadata{};
    metadata.ppq = division;
    std::map<std::pair<int, int>, std::deque<std::size_t>> active;
    std::vector<NoteAction> noteActions;
    std::int64_t lastTick = 0;
    for (int tr = 0; tr < tracks; ++tr) {
        std::uint32_t trackSize = 0;
        if (!file.take(4, tag) || !tagIs(tag, "MTrk") || !file.be32(trackSize) ||
            !file.take(trackSize, tag))
            return badSmf("A MIDI track is truncated.");
        Reader track{tag};
        std::int64_t tick = 0;
        std::uint8_t running = 0;
        while (track.at < track.bytes.size()) {
            std::uint32_t delta = 0;
            std::uint8_t first = 0;
            if (!track.vlq(delta) || !track.byte(first))
                return badSmf("A MIDI event is truncated.");
            tick += delta;
            lastTick = std::max(lastTick, tick);
            if (first == 0xff) {
                std::uint8_t type = 0;
                std::uint32_t n = 0;
                std::span<const std::uint8_t> payload;
                if (!track.byte(type) || !track.vlq(n) || !track.take(n, payload))
                    return badSmf("A meta event is truncated.");
                running = 0;
                if (type == 0x51 && n == 3) {
                    const std::uint32_t micros = (std::uint32_t(payload[0]) << 16) |
                                                 (std::uint32_t(payload[1]) << 8) | payload[2];
                    if (micros != 0) metadata.bpm = 60000000.0 / micros;
                } else if (type == 0x58 && n >= 2 && payload[1] < 31) {
                    metadata.meter = Meter{payload[0], 1 << payload[1]};
                } else if (type == 0x03) {
                    metadata.trackNames.emplace_back(reinterpret_cast<const char*>(payload.data()),
                                                     payload.size());
                } else if (type != 0x2f) {
                    ++metadata.skippedEvents;
                }
                if (type == 0x2f) break;
                continue;
            }
            if (first == 0xf0 || first == 0xf7) {
                std::uint32_t n = 0;
                std::span<const std::uint8_t> payload;
                if (!track.vlq(n) || !track.take(n, payload))
                    return badSmf("A sysex event is truncated.");
                Sysex sx;
                sx.tick = tick;
                sx.bytes.push_back(first);
                sx.bytes.insert(sx.bytes.end(), payload.begin(), payload.end());
                out.sysex.push_back(std::move(sx));
                running = 0;
                continue;
            }
            std::uint8_t status = first;
            std::uint8_t data1 = 0;
            if (first < 0x80) {
                if (running == 0) return badSmf("Running status has no preceding channel status.");
                status = running;
                data1 = first;
            } else {
                if (first >= 0xf0) return badSmf("An unsupported MIDI system event was found.");
                running = first;
                if (!track.byte(data1)) return badSmf("A channel event is truncated.");
            }
            const int kind = status >> 4;
            std::uint8_t data2 = 0;
            if (kind != 0xc && kind != 0xd && !track.byte(data2))
                return badSmf("A channel event is truncated.");
            if (data1 >= 128 || data2 >= 128)
                return badSmf("A channel event has an invalid data byte.");
            const std::uint8_t ch = status & 0x0f;
            if (kind == 0x9 && data2 != 0) {
                noteActions.push_back({tick, tr, noteActions.size(), ch, data1, data2, true});
            } else if (kind == 0x8 || kind == 0x9) {
                noteActions.push_back({tick, tr, noteActions.size(), ch, data1,
                                       static_cast<std::uint8_t>(kind == 0x8 ? data2 : 0), false});
            } else if (kind == 0xa) {
                out.controls.push_back({tick, ControlKind::PolyPressure, ch, data1, data2});
            } else if (kind == 0xb) {
                out.controls.push_back({tick, ControlKind::CC, ch, data1, data2});
            } else if (kind == 0xc) {
                out.controls.push_back({tick, ControlKind::ProgramChange, ch, 0, data1});
            } else if (kind == 0xd) {
                out.controls.push_back({tick, ControlKind::ChannelPressure, ch, 0, data1});
            } else if (kind == 0xe) {
                out.controls.push_back({tick, ControlKind::PitchBend, ch, 0,
                                        static_cast<std::int32_t>((data2 << 7) | data1) - 8192});
            }
        }
    }
    std::stable_sort(
        noteActions.begin(), noteActions.end(), [](const NoteAction& a, const NoteAction& b) {
            return std::tie(a.tick, a.track, a.order) < std::tie(b.tick, b.track, b.order);
        });
    for (const auto& action : noteActions) {
        auto& queue = active[{action.channel, action.pitch}];
        if (action.on) {
            out.notes.push_back(
                Note{action.tick, 0, action.channel, action.pitch, action.velocity, 0});
            queue.push_back(out.notes.size() - 1);
        } else if (!queue.empty()) {
            auto& note = out.notes[queue.front()];
            queue.pop_front();
            note.length = action.tick - note.start;
            note.offVelocity = action.velocity;
        } else {
            ++metadata.skippedEvents;
        }
    }
    for (auto& [key, queue] : active) {
        (void)key;
        for (const auto index : queue) out.notes[index].length = lastTick - out.notes[index].start;
    }
    out.length = lastTick;
    out.canonicalize();
    if (info != nullptr) *info = std::move(metadata);
    return out;
}

std::vector<std::uint8_t> writeSmf(const Sequence& source, const std::optional<Meter>& meter,
                                   double bpm) {
    Sequence sequence = source;
    sequence.canonicalize();
    std::vector<Event> events;
    if (meter && meter->denominator > 0) {
        int power = 0;
        for (int d = meter->denominator; d > 1; d >>= 1) ++power;
        events.push_back({0,
                          0,
                          {0xff, 0x58, 0x04, static_cast<std::uint8_t>(meter->numerator),
                           static_cast<std::uint8_t>(power), 24, 8}});
    }
    if (std::isfinite(bpm) && bpm > 0) {
        auto micros =
            static_cast<std::uint32_t>(std::clamp(std::llround(60000000.0 / bpm), 1LL, 16777215LL));
        events.push_back(
            {0,
             0,
             {0xff, 0x51, 0x03, static_cast<std::uint8_t>(micros >> 16),
              static_cast<std::uint8_t>(micros >> 8), static_cast<std::uint8_t>(micros)}});
    }
    for (const auto& note : sequence.notes) {
        events.push_back(
            {note.start,
             3,
             {static_cast<std::uint8_t>(0x90 | note.channel), note.pitch, note.velocity}});
        events.push_back(
            {note.start + note.length,
             note.length == 0 ? 4 : 1,
             {static_cast<std::uint8_t>(0x80 | note.channel), note.pitch, note.offVelocity}});
    }
    for (const auto& c : sequence.controls) {
        Bytes data;
        const auto ch = c.channel;
        switch (c.kind) {
            case ControlKind::CC:
                data = {static_cast<std::uint8_t>(0xb0 | ch), c.number,
                        static_cast<std::uint8_t>(c.value)};
                break;
            case ControlKind::ProgramChange:
                data = {static_cast<std::uint8_t>(0xc0 | ch), static_cast<std::uint8_t>(c.value)};
                break;
            case ControlKind::ChannelPressure:
                data = {static_cast<std::uint8_t>(0xd0 | ch), static_cast<std::uint8_t>(c.value)};
                break;
            case ControlKind::PolyPressure:
                data = {static_cast<std::uint8_t>(0xa0 | ch), c.number,
                        static_cast<std::uint8_t>(c.value)};
                break;
            case ControlKind::PitchBend: {
                const int value = c.value + 8192;
                data = {static_cast<std::uint8_t>(0xe0 | ch),
                        static_cast<std::uint8_t>(value & 127),
                        static_cast<std::uint8_t>((value >> 7) & 127)};
                break;
            }
        }
        events.push_back({c.tick, 2, std::move(data)});
    }
    for (const auto& sx : sequence.sysex) {
        if (sx.bytes.empty()) continue;
        Bytes data{sx.bytes.front()};
        vlq(data, static_cast<std::uint32_t>(sx.bytes.size() - 1));
        data.insert(data.end(), sx.bytes.begin() + 1, sx.bytes.end());
        events.push_back({sx.tick, 2, std::move(data)});
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return std::tie(a.tick, a.order) < std::tie(b.tick, b.order);
    });
    Bytes track;
    std::int64_t previous = 0;
    for (const auto& e : events) {
        emitDelta(track, e.tick - previous);
        track.insert(track.end(), e.data.begin(), e.data.end());
        previous = e.tick;
    }
    const std::int64_t end = std::max(previous, sequence.length);
    emitDelta(track, end - previous);
    track.insert(track.end(), {0xff, 0x2f, 0x00});
    Bytes out{'M', 'T', 'h', 'd'};
    be32(out, 6);
    be16(out, 0);
    be16(out, 1);
    be16(out, static_cast<std::uint16_t>(sequence.ppq));
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    be32(out, static_cast<std::uint32_t>(track.size()));
    out.insert(out.end(), track.begin(), track.end());
    return out;
}

}  // namespace supple::midi
