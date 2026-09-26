#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include <supple/clap/semantic_params.h>
#include <supple/host/clap_host.h>
#include <supple/json/json.h>

using namespace supple;
using namespace supple::host;

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;

PluginInfo find(const std::string& id) {
    auto list = scan(std::span<const std::filesystem::path>(&kFixture, 1));
    REQUIRE(list);
    for (auto& p : *list)
        if (p.id == id) return p;
    FAIL("fixture plugin not found: ", id);
    return {};
}

struct Buffers {
    std::vector<float> l, r, ol, orr;
    float* in[2];
    float* out[2];
    explicit Buffers(std::uint32_t n) : l(n), r(n), ol(n), orr(n) {
        in[0] = l.data();
        in[1] = r.data();
        out[0] = ol.data();
        out[1] = orr.data();
    }
    ProcessBlock block(std::uint32_t n, std::span<const Event> events = {}, bool withInput = true) {
        ProcessBlock b;
        b.inputs = withInput ? in : nullptr;
        b.inputChannels = withInput ? 2 : 0;
        b.outputs = out;
        b.outputChannels = 2;
        b.frames = n;
        b.events = events;
        return b;
    }
};

double gainClap(double db) {
    // The fixture uses ADR-0010 normalization: linear curve over -60..12 dB.
    return (db + 60.0) / 72.0;
}

}  // namespace

TEST_CASE("scan finds both fixtures with their capabilities") {
    auto list = scan(std::span<const std::filesystem::path>(&kFixture, 1));
    REQUIRE(list);
    REQUIRE(list->size() == 2);
    const auto gain = find("com.suppleaudio.test.gain");
    CHECK_FALSE(gain.instrument);
    CHECK(gain.semantic);
    CHECK(gain.name == "Test Gain");
    const auto sine = find("com.suppleaudio.test.sine");
    CHECK(sine.instrument);
    CHECK_FALSE(sine.semantic);
    auto bad = Instance::create(PluginInfo{.id = "nope", .path = kFixture}, {});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "host.create");
}

TEST_CASE("gain: ports, params, sample-accurate automation") {
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(inst);
    auto& g = **inst;
    CHECK(g.audioInputChannels() == 2);
    CHECK(g.audioOutputChannels() == 2);
    CHECK_FALSE(g.acceptsNotes());
    auto ps = g.params();
    REQUIRE(ps.size() == 2);
    CHECK(ps[0].name == "Gain");
    CHECK((ps[1].flags & CLAP_PARAM_IS_STEPPED) != 0);
    CHECK(*g.paramText(0, gainClap(-6)) == "-6.0 dB");
    CHECK(*g.paramFromText(0, "-6 dB") == doctest::Approx(gainClap(-6)));
    CHECK_FALSE(g.paramFromText(0, "loud"));

    REQUIRE(g.activate(48000, 1, 512).ok());
    g.startProcessing();
    Buffers buf(256);
    for (std::uint32_t i = 0; i < 256; ++i) buf.l[i] = buf.r[i] = 0.5f;
    Event e{.kind = Event::Kind::Param, .time = 100, .paramId = 0, .value = gainClap(-60)};
    REQUIRE(g.process(buf.block(256, std::span<const Event>(&e, 1))));
    CHECK(static_cast<double>(buf.ol[99]) == doctest::Approx(0.5));
    CHECK(buf.ol[100] == 0.0f);  // -60 dB is off-at-min: silence exactly from sample 100
    // A queued change applies at the start of the next block.
    REQUIRE(g.setParam(0, gainClap(0)).ok());
    REQUIRE(g.process(buf.block(256)));
    CHECK(static_cast<double>(buf.ol[0]) == doctest::Approx(0.5));
    g.stopProcessing();
    g.deactivate();
    // Inactive: flushed immediately on the main thread.
    REQUIRE(g.setParam(0, gainClap(-12)).ok());
    CHECK(*g.paramValue(0) == doctest::Approx(gainClap(-12)));
}

TEST_CASE("gain: state round trip across instances") {
    auto a = Instance::create(find("com.suppleaudio.test.gain"), {});
    auto b = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE((*a)->setParam(0, gainClap(-3)).ok());
    auto state = (*a)->saveState();
    REQUIRE(state);
    REQUIRE((*b)->loadState(*state).ok());
    CHECK(*(*b)->paramValue(0) == doctest::Approx(gainClap(-3)));
    std::vector<std::uint8_t> junk = {1, 2, 3};
    CHECK_FALSE((*b)->loadState(junk).ok());
}

TEST_CASE("gain: semantic schema, patches, errors, validate-only") {
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(inst);
    auto& g = **inst;
    auto schema = g.semanticSchema();
    REQUIRE(schema);
    CHECK(schema->find("params")->find("gain")->find("unit")->asString() == "dB");
    CHECK(*g.getPatch(true) ==
          "{\n  \"schema\": \"supple.test-gain-patch/1.0\",\n  \"params\": {}\n}\n");
    REQUIRE(g.setPatch(R"({"params": {"gain": "-4.5 dB"}})", CLAP_SUPPLE_SET_MERGE).ok());
    CHECK(*g.paramValue(0) == doctest::Approx(gainClap(-4.5)));
    CHECK(g.getPatch(true)->find("\"gain\": -4.5") != std::string::npos);
    CHECK(*g.describe(true) == "Gain: Gain -4.5 dB");
    auto bad = g.setPatch(R"({"params": {"gian": 1}})", CLAP_SUPPLE_SET_MERGE);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == "params.unknown_key");
    CHECK(bad.error().path == "/params/gian");
    CHECK(bad.error().hint.find("gain") != std::string::npos);
    REQUIRE(g.setPatch(R"({"params": {"gain": 6}})", CLAP_SUPPLE_SET_VALIDATE_ONLY).ok());
    CHECK(*g.paramValue(0) == doctest::Approx(gainClap(-4.5)));  // unchanged
}

TEST_CASE("gain: latency change requests a restart; serviceMainThread reactivates") {
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(inst);
    auto& g = **inst;
    REQUIRE(g.activate(48000, 1, 256).ok());
    CHECK(g.latency() == 0);
    REQUIRE(g.setPatch(R"({"params": {"latency": "short"}})", CLAP_SUPPLE_SET_MERGE).ok());
    CHECK(g.serviceMainThread());
    CHECK(g.active());
    CHECK(g.latency() == 64);
    g.startProcessing();
    Buffers buf(128);
    buf.l[0] = buf.r[0] = 1.0f;
    REQUIRE(g.process(buf.block(128)));
    CHECK(buf.ol[0] == 0.0f);
    CHECK(static_cast<double>(buf.ol[64]) == doctest::Approx(1.0));
    g.stopProcessing();
}

TEST_CASE("sine: notes at sample offsets, note end events, MIDI dialect") {
    auto inst = Instance::create(find("com.suppleaudio.test.sine"), {});
    REQUIRE(inst);
    auto& s = **inst;
    CHECK(s.audioInputChannels() == 0);
    CHECK(s.acceptsNotes());
    REQUIRE(s.activate(48000, 1, 512).ok());
    s.startProcessing();
    Buffers buf(512);
    const Event events[] = {
        {.kind = Event::Kind::NoteOn, .time = 10, .key = 69, .noteId = 7, .velocity = 1.0},
        {.kind = Event::Kind::NoteOff, .time = 300, .key = 69, .noteId = 7},
    };
    std::vector<OutputEvent> storage(16);
    OutputEvents out{storage};
    REQUIRE(s.process(buf.block(512, events, false), &out));
    for (int i = 0; i <= 10; ++i) CHECK(buf.ol[i] == 0.0f);  // phase 0 at the note start
    double energy = 0;
    for (int i = 11; i < 300; ++i)
        energy += static_cast<double>(buf.ol[i]) * static_cast<double>(buf.ol[i]);
    CHECK(energy > 1.0);
    for (int i = 300; i < 512; ++i) CHECK(buf.ol[i] == 0.0f);
    REQUIRE(out.count == 1);
    CHECK(out.storage[0].kind == OutputEvent::Kind::NoteEnd);
    CHECK(out.storage[0].time == 300);
    CHECK(out.storage[0].noteId == 7);
    // Raw MIDI events also reach the plugin.
    Event midiOn{.kind = Event::Kind::Midi, .time = 0};
    midiOn.midi[0] = 0x90;
    midiOn.midi[1] = 60;
    midiOn.midi[2] = 100;
    REQUIRE(s.process(buf.block(64, std::span<const Event>(&midiOn, 1), false)));
    double e2 = 0;
    for (int i = 0; i < 64; ++i)
        e2 += static_cast<double>(buf.ol[i]) * static_cast<double>(buf.ol[i]);
    CHECK(e2 > 0.0);
    s.stopProcessing();
}

namespace {
bool rtBlock(Instance& g, const ProcessBlock& b, OutputEvents& out) SUPPLE_RT {
    return g.process(b, &out);
}
}  // namespace

TEST_CASE("the process path is real-time safe" * doctest::description("rt")) {
    auto inst = Instance::create(find("com.suppleaudio.test.sine"), {});
    REQUIRE(inst);
    REQUIRE((*inst)->activate(48000, 1, 256).ok());
    (*inst)->startProcessing();
    Buffers buf(256);
    std::vector<OutputEvent> storage(64);
    OutputEvents out{storage};
    const Event events[] = {{.kind = Event::Kind::NoteOn, .time = 3, .key = 60, .velocity = 0.8},
                            {.kind = Event::Kind::NoteOff, .time = 200, .key = 60}};
    bool ok = true;
    for (int i = 0; i < 50; ++i) ok = ok && rtBlock(**inst, buf.block(256, events, false), out);
    CHECK(ok);
    (*inst)->stopProcessing();
}
