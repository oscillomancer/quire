// Canonical MIDI item model and Standard MIDI File conversion. Non-real-time.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <supple/base/result.h>

namespace supple::midi {

struct Note {
    std::int64_t start = 0;
    std::int64_t length = 0;
    std::uint8_t channel = 0;
    std::uint8_t pitch = 60;
    std::uint8_t velocity = 96;
    std::uint8_t offVelocity = 0;
    friend bool operator==(const Note&, const Note&) = default;
};

enum class ControlKind : std::uint8_t {
    CC,
    ProgramChange,
    ChannelPressure,
    PolyPressure,
    PitchBend
};
struct Control {
    std::int64_t tick = 0;
    ControlKind kind = ControlKind::CC;
    std::uint8_t channel = 0;
    std::uint8_t number = 0;
    std::int32_t value = 0;
    friend bool operator==(const Control&, const Control&) = default;
};

struct Sysex {
    std::int64_t tick = 0;
    std::vector<std::uint8_t> bytes;
    friend bool operator==(const Sysex&, const Sysex&) = default;
};

struct Meter {
    int numerator = 4;
    int denominator = 4;
    friend bool operator==(const Meter&, const Meter&) = default;
};

struct Sequence {
    int ppq = 960;
    std::int64_t length = 0;
    std::vector<Note> notes;
    std::vector<Control> controls;
    std::vector<Sysex> sysex;

    void canonicalize();
    friend bool operator==(const Sequence&, const Sequence&) = default;
};

struct SmfInfo {
    int ppq = 960;
    int format = 0;
    int trackCount = 0;
    std::vector<std::pair<std::int64_t, Meter>> meters;
    std::vector<std::pair<std::int64_t, double>> tempos;
    std::vector<std::string> trackNames;
    int skippedEvents = 0;
};

struct SmfReadOptions {
    std::optional<int> track;
};
Result<Sequence> readSmf(std::span<const std::uint8_t> bytes, SmfInfo* info = nullptr,
                         const SmfReadOptions& options = {});
// Encoded files are limited to 16 MiB (including headers and padding meta
// events for long deltas). Larger output returns midi.smf_too_large.
inline constexpr std::size_t kMaxSmfBytes = 16 * 1024 * 1024;
Result<std::vector<std::uint8_t>> writeSmf(
    const Sequence& sequence, const std::vector<std::pair<std::int64_t, Meter>>& meters,
    const std::vector<std::pair<std::int64_t, double>>& tempos);

}  // namespace supple::midi
