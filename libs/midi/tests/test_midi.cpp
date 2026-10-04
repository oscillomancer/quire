#include <supple/midi/midi.h>

#include <doctest/doctest.h>
#include <limits>

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
    auto encoded = writeSmf(s, {{0, Meter{4, 4}}}, {{0, 120.0}});
    REQUIRE(encoded.ok());
    SmfInfo info;
    auto decoded = readSmf(*encoded, &info);
    REQUIRE(decoded.ok());
    s.canonicalize();
    CHECK(decoded.value() == s);
    CHECK(info.meters == std::vector<std::pair<std::int64_t, Meter>>{{0, Meter{4, 4}}});
    REQUIRE(info.tempos.size() == 1);
    CHECK(info.tempos[0].second == doctest::Approx(120.0));
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
    CHECK(info.format == 1);
    CHECK(info.trackCount == 2);
    auto selected = readSmf(smf, nullptr, SmfReadOptions{0});
    REQUIRE(selected.ok());
    REQUIRE(selected->notes.size() == 1);
    CHECK(selected->notes[0].length == 20);
    auto badTrack = readSmf(smf, nullptr, SmfReadOptions{2});
    REQUIRE_FALSE(badTrack.ok());
    CHECK(badTrack.error().code == "midi.track_range");
}

TEST_CASE("SMF writer validates the clip and metadata") {
    Sequence s;
    s.ppq = 0;
    CHECK_FALSE(writeSmf(s, {}, {}).ok());
    s.ppq = 960;
    s.notes = {{0, 100, 0, 60, 0, 0}};
    s.length = 100;
    CHECK_FALSE(writeSmf(s, {}, {}).ok());
    s.notes[0].velocity = 96;
    CHECK_FALSE(writeSmf(s, {{0, Meter{3, 3}}}, {}).ok());
    CHECK_FALSE(writeSmf(s, {}, {{0, -10.0}}).ok());
    CHECK(writeSmf(s, {{0, Meter{3, 4}}}, {{0, 120.0}}).ok());
}

TEST_CASE("SMF writing rejects unbounded delta padding with a structured error") {
    Sequence source;
    source.length = std::numeric_limits<std::int64_t>::max();
    auto encoded = writeSmf(source, {}, {});
    REQUIRE_FALSE(encoded);
    CHECK(encoded.error().code == "midi.smf_too_large");
    CHECK(encoded.error().path == "/length");
    CHECK_FALSE(encoded.error().hint.empty());
    // A long but bounded delta still round-trips (with padding meta events).
    source.length = 0x0fffffffLL * 3 + 10;
    encoded = writeSmf(source, {}, {});
    REQUIRE(encoded);
    CHECK(encoded->size() <= kMaxSmfBytes);
    const auto decoded = readSmf(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->length == source.length);
}

TEST_CASE("SMF writing enforces the encoded byte budget for payloads") {
    Sequence source;
    source.sysex = {{0, std::vector<std::uint8_t>(kMaxSmfBytes, 0)}};
    source.sysex[0].bytes.front() = 0xf0;
    source.sysex[0].bytes.back() = 0xf7;
    auto encoded = writeSmf(source, {}, {});
    REQUIRE_FALSE(encoded);
    CHECK(encoded.error().code == "midi.smf_too_large");
}
