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
    int ppq = 0;
    std::optional<Meter> meter;
    std::optional<double> bpm;
    std::vector<std::string> trackNames;
    int skippedEvents = 0;
};

Result<Sequence> readSmf(std::span<const std::uint8_t> bytes, SmfInfo* info = nullptr);
std::vector<std::uint8_t> writeSmf(const Sequence& sequence, const std::optional<Meter>& meter,
                                   double bpm);

}  // namespace supple::midi
