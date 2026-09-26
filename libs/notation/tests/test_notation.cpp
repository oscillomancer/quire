#include <supple/notation/notation.h>

#include <doctest/doctest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace supple::notation;

namespace {
std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string dump(const supple::midi::Sequence& sequence) {
    std::ostringstream out;
    out << "ppq " << sequence.ppq << " length " << sequence.length << '\n';
    for (const auto& n : sequence.notes)
        out << "note " << n.start << ' ' << n.length << ' ' << int(n.channel) << ' ' << int(n.pitch)
            << ' ' << int(n.velocity) << ' ' << int(n.offVelocity) << '\n';
    for (const auto& c : sequence.controls)
        out << "control " << c.tick << ' ' << int(c.kind) << ' ' << int(c.channel) << ' '
            << int(c.number) << ' ' << c.value << '\n';
    for (const auto& sx : sequence.sysex) {
        out << "sysex " << sx.tick;
        for (auto byte : sx.bytes) out << ' ' << int(byte);
        out << '\n';
    }
    return out.str();
}
}  // namespace

TEST_CASE("valid notation corpus matches event listings") {
    const auto root = std::filesystem::path(NOTATION_CORPUS_DIR) / "valid";
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".note") continue;
        ++count;
        auto result = compile(readText(entry.path()));
        INFO(entry.path().string());
        std::string errorInfo;
        if (!result) {
            errorInfo = result.error().toString();
            for (const auto& detail : result.error().details) errorInfo += "\n" + detail.toString();
        }
        INFO(errorInfo);
        REQUIRE(result.ok());
        const auto expectedPath =
            entry.path().parent_path() / (entry.path().stem().string() + ".events");
        const auto actual = dump(result->sequence);
        if (std::getenv("SUPPLE_UPDATE_NOTATION_GOLDENS")) {
            std::ofstream output(expectedPath);
            output << actual;
        } else
            CHECK(actual == readText(expectedPath));
    }
    CHECK(count >= 40);
}

TEST_CASE("invalid notation corpus has precise errors") {
    const auto root = std::filesystem::path(NOTATION_CORPUS_DIR) / "invalid";
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".note") continue;
        ++count;
        const auto expectPath =
            entry.path().parent_path() / (entry.path().stem().string() + ".expect");
        std::istringstream expectation(readText(expectPath));
        std::string code, hint;
        int line = 0, column = 0;
        expectation >> code >> line >> column >> hint;
        auto result = compile(readText(entry.path()));
        INFO(entry.path().string());
        REQUIRE_FALSE(result.ok());
        std::vector<supple::Error> errors;
        if (result.error().details.empty())
            errors.push_back(result.error());
        else
            errors = result.error().details;
        bool matched = false;
        for (const auto& e : errors)
            if (e.code == code && e.loc && e.loc->line == line && e.loc->column == column &&
                !e.hint.empty() && e.hint.find(hint) != std::string::npos)
                matched = true;
        CHECK(matched);
    }
    CHECK(count >= 30);
}

TEST_CASE("compile a simple voice") {
    auto result =
        compile("notation 1\nppq 960\nmeter 4/4\nvoice bass ch 1\n1: C4:q D4:q E4:q F4:q\n");
    REQUIRE(result.ok());
    CHECK(result->sequence.notes.size() == 4);
    CHECK(result->sequence.length == 3840);
}

TEST_CASE("thirty-second duration is distinct from exact tick suffix") {
    auto result = compile("voice lead\n1: C4:t D4:120t r:*\n");
    REQUIRE(result.ok());
    REQUIRE(result->sequence.notes.size() == 2);
    CHECK(result->sequence.notes[0].length == 120);
    CHECK(result->sequence.notes[1].length == 120);
}

TEST_CASE("implicit main voice matches an explicit declaration") {
    const auto implicit = compile("notation 1\n1: C4:q E4:q G4:h\n");
    const auto explicitVoice = compile("notation 1\nvoice main\n1: C4:q E4:q G4:h\n");
    REQUIRE(implicit.ok());
    REQUIRE(explicitVoice.ok());
    CHECK(implicit->sequence == explicitVoice->sequence);
    auto written = decompile(implicit->sequence);
    REQUIRE(written.ok());
    CHECK(written->find("voice ") != std::string::npos);
}

TEST_CASE("numbered bar outside a voice suggests a voice declaration") {
    auto result = compile("lane cc 74\n1: C4:w\n");
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().hint.find("voice NAME") != std::string::npos);
}

TEST_CASE("exact item length and repeated lane point") {
    auto result = compile("notation 1\nppq 960\nlength 0+1t\nlane cc 1\n1.1 10\n1.1 20\n");
    REQUIRE(result.ok());
    CHECK(result->sequence.length == 1);
    CHECK(result->sequence.controls.size() == 2);
}

TEST_CASE("note round trip") {
    supple::midi::Sequence s;
    s.length = 900;
    s.notes = {{50, 800, 0, 60, 100, 20}, {100, 200, 0, 64, 70, 0}};
    auto written = decompile(s);
    REQUIRE(written.ok());
    auto compiled = compile(*written);
    REQUIRE(compiled.ok());
    s.canonicalize();
    CHECK(compiled->sequence == s);
}

TEST_CASE("decompiler emits grid and copy lines when exact") {
    supple::midi::Sequence drums;
    drums.length = 3840;
    drums.notes = {{0, 240, 9, 36, 100, 0}, {960, 240, 9, 36, 100, 0}};
    auto grid = decompile(drums);
    REQUIRE(grid.ok());
    CHECK(grid->find("voice drums ch 10 grid s") != std::string::npos);
    auto compiledGrid = compile(*grid);
    REQUIRE(compiledGrid.ok());
    drums.canonicalize();
    CHECK(compiledGrid->sequence == drums);

    supple::midi::Sequence repeated;
    repeated.length = 11520;
    repeated.notes = {
        {0, 3840, 0, 60, 96, 0}, {3840, 3840, 0, 60, 96, 0}, {7680, 3840, 0, 60, 96, 0}};
    auto copied = decompile(repeated);
    REQUIRE(copied.ok());
    CHECK(copied->find("2-3 = 1") != std::string::npos);
    auto compiledCopy = compile(*copied);
    REQUIRE(compiledCopy.ok());
    repeated.canonicalize();
    CHECK(compiledCopy->sequence == repeated);
}

TEST_CASE("same-tick lane writes retain order through decompile") {
    supple::midi::Sequence s;
    s.length = 1;
    s.controls = {{0, supple::midi::ControlKind::CC, 0, 74, 20},
                  {0, supple::midi::ControlKind::CC, 0, 74, 90}};
    auto written = decompile(s);
    REQUIRE(written.ok());
    CHECK(written->find("1.1 20\n1.1 90") != std::string::npos);
    auto compiled = compile(*written);
    REQUIRE(compiled.ok());
    CHECK(compiled->sequence == s);
}

TEST_CASE("fractional denominator beats retain integer tick controls") {
    for (int ppq : {1, 3}) {
        supple::midi::Sequence s;
        s.ppq = ppq;
        s.length = ppq * 3;
        s.controls = {{2, supple::midi::ControlKind::CC, 0, 1, 80}};
        DecompileOptions options;
        options.meterAt = [](int) { return supple::midi::Meter{6, 8}; };
        CompileContext context;
        context.meterAt = options.meterAt;
        auto written = decompile(s, options);
        REQUIRE(written.ok());
        CHECK(written->find("1.1+2t 80") != std::string::npos);
        auto compiled = compile(*written, context);
        REQUIRE(compiled.ok());
        CHECK(compiled->sequence == s);
    }
}

TEST_CASE("round-trip random canonical sequences") {
    constexpr std::uint32_t seed = 0x5a17c0de;
    std::mt19937 rng(seed);
    const int ppqs[] = {96, 480, 960, 1920};
    const supple::midi::Meter meters[] = {{4, 4}, {3, 4}, {6, 8}, {5, 8}};
    for (int iteration = 0; iteration < 2000; ++iteration) {
        supple::midi::Sequence s;
        s.ppq = ppqs[rng() % 4];
        const auto meter = meters[rng() % 4];
        const auto span = s.ppq * 4 * meter.numerator / meter.denominator;
        const int noteCount = rng() % 9;
        for (int i = 0; i < noteCount; ++i)
            s.notes.push_back({static_cast<std::int64_t>(rng() % (span * 3)),
                               static_cast<std::int64_t>(rng() % (span * 2)),
                               static_cast<std::uint8_t>(rng() % 16),
                               static_cast<std::uint8_t>(rng() % 128),
                               static_cast<std::uint8_t>(1 + rng() % 127),
                               static_cast<std::uint8_t>(rng() % 128)});
        const int controlCount = rng() % 9;
        for (int i = 0; i < controlCount; ++i) {
            const auto kind = static_cast<supple::midi::ControlKind>(rng() % 5);
            s.controls.push_back(
                {static_cast<std::int64_t>(rng() % (span * 3)), kind,
                 static_cast<std::uint8_t>(rng() % 16),
                 static_cast<std::uint8_t>(kind == supple::midi::ControlKind::CC ||
                                                   kind == supple::midi::ControlKind::PolyPressure
                                               ? rng() % 128
                                               : 0),
                 kind == supple::midi::ControlKind::PitchBend
                     ? static_cast<int>(rng() % 16384) - 8192
                     : static_cast<int>(rng() % 128)});
        }
        if (rng() % 4 == 0)
            s.sysex.push_back({static_cast<std::int64_t>(rng() % (span * 3)),
                               {0xf0, 0x7e, static_cast<std::uint8_t>(rng() % 128), 0xf7}});
        std::int64_t end = 0;
        for (const auto& n : s.notes) end = std::max(end, n.start + n.length);
        for (const auto& c : s.controls) end = std::max(end, c.tick);
        for (const auto& x : s.sysex) end = std::max(end, x.tick);
        s.length = end + static_cast<std::int64_t>(rng() % (span + 1));
        s.canonicalize();
        DecompileOptions options;
        options.meterAt = [meter](int) { return meter; };
        CompileContext context;
        context.meterAt = options.meterAt;
        auto text = decompile(s, options);
        INFO("seed=", seed, " iteration=", iteration);
        REQUIRE(text.ok());
        auto compiled = compile(*text, context);
        INFO(*text);
        std::string errorInfo;
        if (!compiled) {
            errorInfo = compiled.error().toString();
            for (const auto& detail : compiled.error().details)
                errorInfo += "\n" + detail.toString();
        }
        INFO(errorInfo);
        REQUIRE(compiled.ok());
        CHECK(compiled->sequence == s);
    }
}
