#include <supple/midi/midi.h>

#include <doctest/doctest.h>

using namespace supple::midi;

TEST_CASE("canonical order retains controller writes at one address") {
    Sequence s;
    s.notes = {{100, 10, 1, 60, 90, 0}, {0, 10, 0, 60, 90, 0}, {0, 10, 0, 60, 90, 0}};
    s.controls = {{0, ControlKind::CC, 0, 1, 20}, {0, ControlKind::CC, 0, 1, 10}};
    s.canonicalize();
    CHECK(s.notes.size() == 2);
    CHECK(s.notes[0].start == 0);
    CHECK(s.controls[0].value == 20);
    CHECK(s.controls[1].value == 10);
}

TEST_CASE("SMF format zero round trip") {
    Sequence s;
    s.length = 3840;
    s.notes = {{0, 960, 0, 60, 100, 55}, {1000, 0, 9, 36, 60, 0}};
    s.controls = {{500, ControlKind::CC, 0, 74, 127},
                  {900, ControlKind::PitchBend, 0, 0, -300},
                  {1000, ControlKind::ProgramChange, 0, 0, 5}};
    s.sysex = {{1200, {0xf0, 0x7e, 0x7f, 0xf7}}};
    auto encoded = writeSmf(s, Meter{4, 4}, 120.0);
    SmfInfo info;
    auto decoded = readSmf(encoded, &info);
    REQUIRE(decoded.ok());
    s.canonicalize();
    CHECK(decoded.value() == s);
    CHECK(info.meter == Meter{4, 4});
    REQUIRE(info.bpm.has_value());
    CHECK(*info.bpm == doctest::Approx(120.0));
}

TEST_CASE("running status, zero-velocity off, and unterminated note") {
    const std::vector<std::uint8_t> smf{
        'M', 'T', 'h', 'd', 0, 0,    0,  6,   0,    0,  0,  1,    3,  0xc0, 'M',  'T',  'r',  'k',
        0,   0,   0,   14,  0, 0x90, 60, 100, 0x10, 62, 90, 0x10, 60, 0,    0x10, 0xff, 0x2f, 0};
    auto result = readSmf(smf);
    REQUIRE(result.ok());
    REQUIRE(result->notes.size() == 2);
    CHECK(result->notes[0].length == 32);
    CHECK(result->notes[1].length == 32);
}

TEST_CASE("format one merges tracks before pairing notes") {
    const std::vector<std::uint8_t> smf{
        'M', 'T', 'h', 'd', 0,   0, 0,    6,  0,   1,  0,    2,    3,  0xc0, 'M',  'T',  'r',
        'k', 0,   0,   0,   13,  0, 0x90, 60, 100, 20, 0xff, 0x03, 1,  'A',  0,    0xff, 0x2f,
        0,   'M', 'T', 'r', 'k', 0, 0,    0,  8,   10, 0x80, 60,   40, 0,    0xff, 0x2f, 0};
    SmfInfo info;
    auto result = readSmf(smf, &info);
    REQUIRE(result.ok());
    REQUIRE(result->notes.size() == 1);
    CHECK(result->notes[0].length == 10);
    CHECK(result->notes[0].offVelocity == 40);
    CHECK(result->length == 20);
    CHECK(info.trackNames == std::vector<std::string>{"A"});
}
