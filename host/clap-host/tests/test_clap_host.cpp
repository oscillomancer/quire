#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <supple/clap/semantic_params.h>
#include <supple/host/clap_host.h>
#include <supple/json/json.h>

using namespace supple;
using namespace supple::host;

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;
const std::filesystem::path kVst3Fixture = FIXTURE_VST3_PATH;

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

TEST_CASE("VST3 fixture scans, processes, and restores state") {
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    REQUIRE(found->size() == 3);
    CHECK((*found)[0].format == "vst3");
    CHECK((*found)[0].id.rfind("vst3:", 0) == 0);
    CHECK((*found)[0].version == "2.0");
    auto gain = std::find_if(found->begin(), found->end(),
                             [](const PluginInfo& p) { return p.name == "Test VST3 Gain"; });
    REQUIRE(gain != found->end());
    auto made = Instance::create(*gain, {});
    REQUIRE(made);
    auto& instance = **made;
    REQUIRE(instance.params().size() == 1);
    REQUIRE(instance.setParam(1, 0.25).ok());
    CHECK(*instance.paramValue(1) == doctest::Approx(0.25));
    auto text = instance.paramText(1, 0.25);
    REQUIRE(text);
    auto parsed = instance.paramFromText(1, *text);
    REQUIRE(parsed);
    CHECK(*parsed == doctest::Approx(0.25));
    auto state = instance.saveState();
    REQUIRE_MESSAGE(state, (state ? "" : state.error().message));
    REQUIRE(instance.setParam(1, 0.75).ok());
    REQUIRE(instance.loadState(*state).ok());
    CHECK(*instance.paramValue(1) == doctest::Approx(0.25));
    REQUIRE(instance.activate(48000, 1, 64).ok());
    REQUIRE(instance.startProcessing());
    Buffers buffer(64);
    std::fill(buffer.l.begin(), buffer.l.end(), 1.0f);
    std::fill(buffer.r.begin(), buffer.r.end(), 1.0f);
    REQUIRE(instance.process(buffer.block(64)));
    CHECK(static_cast<double>(buffer.ol[0]) == doctest::Approx(0.25));
    Event automation{.kind = Event::Kind::Param, .time = 20, .paramId = 1, .value = 0.5};
    REQUIRE(instance.process(buffer.block(64, std::span<const Event>(&automation, 1))));
    CHECK(static_cast<double>(buffer.ol[19]) == doctest::Approx(0.25));
    CHECK(static_cast<double>(buffer.ol[20]) == doctest::Approx(0.5));
    instance.stopProcessing();
    instance.deactivate();
}

TEST_CASE("VST3 fixture notes, latency, and bridge") {
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    auto named = [&](std::string_view name) {
        return std::find_if(found->begin(), found->end(),
                            [&](const PluginInfo& p) { return p.name == name; });
    };
    auto sine = named("Test VST3 Sine");
    REQUIRE(sine != found->end());
    CHECK(sine->instrument);
    auto instrument = Instance::create(*sine, {});
    REQUIRE(instrument);
    REQUIRE((*instrument)->acceptsNotes());
    REQUIRE((*instrument)->activate(48000, 1, 64).ok());
    REQUIRE((*instrument)->startProcessing());
    Buffers buf(64);
    Event note{.kind = Event::Kind::NoteOn, .time = 10, .key = 69, .noteId = 7};
    REQUIRE((*instrument)->process(buf.block(64, std::span<const Event>(&note, 1), false)));
    CHECK(buf.ol[4] == 0);
    CHECK(std::abs(buf.ol[14]) > 0.01f);
    (*instrument)->stopProcessing();
    (*instrument)->deactivate();

    auto latency = named("Test VST3 Latency");
    REQUIRE(latency != found->end());
    auto delay = Instance::create(*latency, {});
    REQUIRE(delay);
    REQUIRE((*delay)->activate(48000, 1, 64).ok());
    CHECK((*delay)->latency() == 0);
    REQUIRE((*delay)->setParam(1, 1).ok());
    CHECK((*delay)->serviceMainThread());
    CHECK((*delay)->takeNotifications().latencyChanged);
    CHECK((*delay)->latency() == 64);
    (*delay)->deactivate();

    auto gain = named("Test VST3 Gain");
    REQUIRE(gain != found->end());
    setBridgeExecutable(BRIDGE_PATH);
    auto bridged = Instance::create(*gain, {}, true);
    REQUIRE_MESSAGE(bridged, (bridged ? "" : bridged.error().message));
    CHECK((*bridged)->raw() == nullptr);
    REQUIRE((*bridged)->setParam(1, 0.2).ok());
    REQUIRE((*bridged)->activate(48000, 1, 64).ok());
    REQUIRE((*bridged)->startProcessing());
    std::fill(buf.l.begin(), buf.l.end(), 1.0f);
    std::fill(buf.r.begin(), buf.r.end(), 1.0f);
    REQUIRE((*bridged)->process(buf.block(64)));
    CHECK(static_cast<double>(buf.ol[0]) == doctest::Approx(0.2));
    (*bridged)->stopProcessing();
    (*bridged)->deactivate();
}

TEST_CASE("scan finds the fixtures with their capabilities") {
    auto list = scan(std::span<const std::filesystem::path>(&kFixture, 1));
    REQUIRE(list);
    REQUIRE(list->size() == 8);
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

TEST_CASE("lifecycle: a restart requested while processing waits until processing stops") {
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(inst);
    auto& g = **inst;
    REQUIRE(g.activate(48000, 1, 256).ok());
    CHECK(g.latency() == 0);
    REQUIRE(g.startProcessing());
    Buffers buf(64);
    Event e;
    e.kind = Event::Kind::Param;
    e.paramId = 1;  // latency: "short" -> the fixture calls request_restart on the audio thread
    e.value = 1.0;
    REQUIRE(g.process(buf.block(64, std::span<const Event>(&e, 1))));
    g.serviceMainThread();  // still processing: must not restart, must keep the request
    CHECK(g.active());
    CHECK(g.latency() == 0);
    g.deactivate();  // refused while processing
    CHECK(g.active());
    bool logged = false;
    for (const auto& line : g.takeLog())
        logged = logged || line.find("deactivate") != std::string::npos;
    CHECK(logged);
    g.stopProcessing();
    CHECK(g.serviceMainThread());
    CHECK(g.active());
    CHECK(g.latency() == 64);
}

TEST_CASE("lifecycle: request_flush while active is served on the audio thread") {
    auto inst = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(inst);
    auto& p = **inst;
    auto flushes = [&] { return p.paramValue(7).value(); };
    const std::uint8_t plain[4] = {0, 0, 0, 0};
    // Inactive: the host flushes on the main thread.
    REQUIRE(p.loadState(plain).ok());
    CHECK_FALSE(p.needsAudioFlush());
    p.serviceMainThread();
    CHECK(flushes() == 1.0);
    // Active: the main thread must not flush; flushParams() on the audio thread does.
    REQUIRE(p.activate(48000, 1, 256).ok());
    REQUIRE(p.loadState(plain).ok());
    CHECK(p.needsAudioFlush());
    p.serviceMainThread();
    CHECK(flushes() == 1.0);
    p.flushParams();
    CHECK(flushes() == 2.0);
    CHECK_FALSE(p.needsAudioFlush());
    // A process call also satisfies the request.
    REQUIRE(p.loadState(plain).ok());
    REQUIRE(p.startProcessing());
    Buffers buf(32);
    REQUIRE(p.process(buf.block(32)));
    CHECK_FALSE(p.needsAudioFlush());
    p.stopProcessing();
}

TEST_CASE("lifecycle: refused start_processing blocks process(); tail is reported") {
    auto inst = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(inst);
    auto& p = **inst;
    const std::uint8_t refuse[4] = {1, 0xe8, 0x03, 0};  // tail 1000 samples
    REQUIRE(p.loadState(refuse).ok());
    CHECK(p.reportsTail());
    CHECK(p.tail() == 1000);
    REQUIRE(p.activate(48000, 1, 256).ok());
    CHECK_FALSE(p.startProcessing());
    Buffers buf(32);
    CHECK_FALSE(p.process(buf.block(32)));
    CHECK_FALSE(Instance::create(find("com.suppleaudio.test.gain"), {}).value()->reportsTail());
}

TEST_CASE("events: queued params wait when the arena is full; overflow is counted") {
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {});
    REQUIRE(inst);
    auto& g = **inst;
    REQUIRE(g.activate(48000, 1, 256).ok());
    REQUIRE(g.startProcessing());
    REQUIRE(g.setParam(0, gainClap(-6)).ok());  // queued (active)
    std::vector<Event> midi(9000);
    for (auto& e : midi) {
        e.kind = Event::Kind::Midi;  // ignored by the gain fixture
        e.midi[0] = 0xb0;
    }
    Buffers buf(256);
    REQUIRE(g.process(buf.block(256, midi)));
    CHECK(g.takeNotifications().droppedEvents == 9000 - 8192);
    CHECK(g.paramValue(0).value() == doctest::Approx(gainClap(0)));  // not delivered yet
    REQUIRE(g.process(buf.block(256)));
    CHECK(g.paramValue(0).value() == doctest::Approx(gainClap(-6)));
    g.stopProcessing();
}

// --- isolation (ADR-0013) ----------------------------------------------------------------

TEST_CASE("isolation: a bridged instance behaves like the in-process one") {
    setBridgeExecutable(BRIDGE_PATH);
    auto local = Instance::create(find("com.suppleaudio.test.gain"), {});
    auto remote = Instance::create(find("com.suppleaudio.test.gain"), {}, true);
    REQUIRE(local);
    REQUIRE_MESSAGE(remote, (remote ? "" : remote.error().toString()));
    Instance& l = **local;
    Instance& r = **remote;
    CHECK(r.isolated());
    CHECK_FALSE(l.isolated());
    CHECK(r.audioInputChannels() == 2);
    CHECK(r.audioOutputChannels() == 2);
    CHECK(r.info().semantic);
    const auto lp = l.params(), rp = r.params();
    REQUIRE(rp.size() == lp.size());
    for (std::size_t i = 0; i < lp.size(); ++i) {
        CHECK(rp[i].id == lp[i].id);
        CHECK(rp[i].name == lp[i].name);
        CHECK(rp[i].max == lp[i].max);
    }
    CHECK(*r.paramText(0, gainClap(-6)) == "-6.0 dB");
    CHECK(*r.paramFromText(0, "-6 dB") == doctest::Approx(gainClap(-6)));
    CHECK_FALSE(r.paramFromText(0, "loud"));
    REQUIRE(r.semanticSchema());
    REQUIRE(r.setPatch(R"({"params": {"gain": "-4.5 dB"}})", CLAP_SUPPLE_SET_MERGE).ok());
    CHECK(*r.describe(true) == "Gain: Gain -4.5 dB");
    auto bad = r.setPatch(R"({"params": {"gian": 1}})", CLAP_SUPPLE_SET_MERGE);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == "params.unknown_key");  // errors cross the bridge intact
    CHECK(bad.error().path == "/params/gian");
    auto state = r.saveState();
    REQUIRE(state);
    REQUIRE(l.loadState(*state).ok());
    CHECK(*l.paramValue(0) == doctest::Approx(gainClap(-4.5)));

    // Latency: a restart in the bridge, reported back.
    REQUIRE(r.activate(48000, 1, 256).ok());
    REQUIRE(l.activate(48000, 1, 256).ok());
    REQUIRE(r.setPatch(R"({"params": {"latency": "short"}})", CLAP_SUPPLE_SET_MERGE).ok());
    REQUIRE(l.setPatch(R"({"params": {"latency": "short"}})", CLAP_SUPPLE_SET_MERGE).ok());
    for (int i = 0; i < 10 && r.latency() != 64; ++i) r.serviceMainThread();
    l.serviceMainThread();
    CHECK(r.latency() == 64);

    // Processing: identical output, sample-accurate events.
    REQUIRE(r.startProcessing());
    REQUIRE(l.startProcessing());
    Buffers bl(256), br(256);
    for (std::uint32_t i = 0; i < 256; ++i)
        bl.l[i] = bl.r[i] = br.l[i] = br.r[i] = std::sin(0.01f * static_cast<float>(i));
    Event e{.kind = Event::Kind::Param, .time = 100, .paramId = 0, .value = gainClap(-60)};
    for (int block = 0; block < 4; ++block) {
        REQUIRE(l.process(bl.block(256, std::span<const Event>(&e, block == 1 ? 1 : 0))));
        REQUIRE(r.process(br.block(256, std::span<const Event>(&e, block == 1 ? 1 : 0))));
        CHECK(bl.ol == br.ol);
        CHECK(bl.orr == br.orr);
    }
    r.reset();
    r.stopProcessing();
    l.stopProcessing();
    CHECK_FALSE(r.failed());
    r.deactivate();
}

TEST_CASE("isolation: a crashing plugin fails its instance, not the host") {
    setBridgeExecutable(BRIDGE_PATH);
    auto inst = Instance::create(find("com.suppleaudio.test.crash"), {}, true);
    REQUIRE_MESSAGE(inst, (inst ? "" : inst.error().toString()));
    Instance& c = **inst;
    REQUIRE(c.activate(48000, 1, 256).ok());
    REQUIRE(c.startProcessing());
    Buffers buf(256);
    std::fill(buf.l.begin(), buf.l.end(), 0.25f);
    std::fill(buf.r.begin(), buf.r.end(), 0.25f);
    REQUIRE(c.process(buf.block(256)));
    CHECK(buf.ol[10] == 0.25f);          // passes audio through
    REQUIRE(c.setParam(1, 1.0).ok());    // crash in the next process()
    REQUIRE(c.process(buf.block(256)));  // the node goes silent; no error for the host
    CHECK(c.failed());
    CHECK(buf.ol[10] == 0.0f);
    CHECK_FALSE(c.failure().empty());
    auto after = c.paramValue(1);
    REQUIRE_FALSE(after);
    CHECK(after.error().code == "host.bridge_failed");
    c.stopProcessing();
    c.deactivate();
}  // destruction after a crash must not hang or throw

TEST_CASE("isolation: a hanging plugin is killed at the deadline") {
    setBridgeExecutable(BRIDGE_PATH);
    auto inst = Instance::create(find("com.suppleaudio.test.crash"), {}, true);
    REQUIRE(inst);
    Instance& c = **inst;
    REQUIRE(c.activate(48000, 1, 256).ok());
    REQUIRE(c.startProcessing());
    REQUIRE(c.setParam(1, 2.0).ok());  // hang in the next process()
    Buffers buf(256);
    REQUIRE(c.process(buf.block(256)));
    CHECK(c.failed());
    CHECK(c.failure().find("stopped responding") != std::string::npos);
    c.stopProcessing();
}

TEST_CASE("isolation: a missing bridge is a clean error") {
    setBridgeExecutable("/nonexistent/supple-bridge");
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {}, true);
    REQUIRE_FALSE(inst);
    CHECK(inst.error().code.rfind("host.bridge", 0) == 0);
    setBridgeExecutable(BRIDGE_PATH);
}
