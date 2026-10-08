#include <doctest/doctest.h>
#include <unistd.h>

#include <dlfcn.h>

#include <fcntl.h>
#include <sched.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>
#include "../src/bridge_client.h"
#include "../src/bridge_protocol.h"

#include <supple/clap/semantic_params.h>
#include <supple/host/clap_host.h>
#include <supple/json/json.h>

extern char** environ;

using namespace supple;
using namespace supple::host;

namespace supple::host {
struct BridgeTestAccess {
    static std::unique_ptr<RemoteInstance> dataPlane(std::shared_ptr<BridgeProcess> process,
                                                     void* arena, bridge::Layout layout) {
        std::unique_ptr<RemoteInstance> instance(new RemoteInstance);
        instance->process_ = std::move(process);
        instance->map_ = arena;
        instance->hdr_ = static_cast<bridge::Header*>(arena);
        instance->layout_ = layout;
        return instance;
    }
    static std::shared_ptr<BridgeProcess> adopt(int socket, pid_t pid) {
        std::shared_ptr<BridgeProcess> process(new BridgeProcess);
        process->sock_ = socket;
        process->pid_ = pid;
        return process;
    }
};
}  // namespace supple::host

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

// The fixture module's controller-call watch (it is dlopen'd by the host and
// by this test). Returns null when the module is not built (VST3 disabled).
struct ControllerWatch {
    int (*count)() = nullptr;
    void (*reset)() = nullptr;
    explicit ControllerWatch(const std::filesystem::path& module) {
        if (void* handle = ::dlopen(module.c_str(), RTLD_NOW)) {
            count = reinterpret_cast<int (*)()>(::dlsym(handle, "fixtureOffThreadControllerCalls"));
            reset = reinterpret_cast<void (*)()>(
                ::dlsym(handle, "fixtureResetOffThreadControllerCalls"));
        }
    }
    bool ok() const { return count && reset; }
};

}  // namespace

TEST_CASE("VST3 fixture scans, processes, and restores state") {
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    REQUIRE(found->size() == 7);
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

TEST_CASE("VST3 plug-ins with a sidechain bus get every bus; kNotImplemented starts processing") {
    // A partial bus layout is refused by strict plug-ins, which then output
    // silence; setProcessing may answer kNotImplemented (sfizz does).
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    auto sidechain = std::find_if(found->begin(), found->end(), [](const PluginInfo& p) {
        return p.name == "Test VST3 Sidechain Gain";
    });
    REQUIRE(sidechain != found->end());
    auto made = Instance::create(*sidechain, {});
    REQUIRE(made);
    auto& instance = **made;
    CHECK(instance.audioInputChannels() == 2);
    REQUIRE(instance.setParam(1, 0.5).ok());
    REQUIRE(instance.activate(48000, 1, 64).ok());
    REQUIRE(instance.startProcessing());
    Buffers buffer(64);
    std::fill(buffer.l.begin(), buffer.l.end(), 1.0f);
    std::fill(buffer.r.begin(), buffer.r.end(), 1.0f);
    REQUIRE(instance.process(buffer.block(64)));
    CHECK(static_cast<double>(buffer.ol[10]) == doctest::Approx(0.5));
    CHECK(static_cast<double>(buffer.orr[10]) == doctest::Approx(0.5));
    instance.stopProcessing();
    instance.deactivate();
}

TEST_CASE("a VST3 instrument's auxiliary output buses are active and reach the block") {
    setBridgeExecutable(BRIDGE_PATH);
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    auto multi = std::find_if(found->begin(), found->end(),
                              [](const PluginInfo& p) { return p.name == "Test VST3 Multi-Out"; });
    REQUIRE(multi != found->end());
    for (bool isolated : {false, true}) {
        CAPTURE(isolated);
        auto made = Instance::create(*multi, {}, isolated);
        REQUIRE(made);
        Instance& p = **made;
        REQUIRE(p.auxOutputPorts().size() == 1);
        CHECK(p.auxOutputPorts()[0].name == "Kick");
        CHECK(p.auxOutputChannels() == 2);
        REQUIRE(p.activate(48000, 1, 64));
        REQUIRE(p.startProcessing());
        Buffers buf(32);
        std::vector<float> kl(32), kr(32);
        float* aux[2] = {kl.data(), kr.data()};
        ProcessBlock b = buf.block(32, {}, false);
        b.auxOutputs = aux;
        b.auxOutputChannels = 2;
        REQUIRE(p.process(b));
        CHECK(buf.ol[0] == 0.125f);
        CHECK(kl[0] == 0.25f);
        CHECK(kr[31] == 0.25f);
        p.stopProcessing();
        p.deactivate();
    }
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
    CHECK_FALSE((*delay)->restartPending());  // not processing: reactivated at once
    // kLatencyChanged while processing: the reactivation VST3 asks for waits.
    REQUIRE((*delay)->startProcessing());
    REQUIRE((*delay)->setParam(1, 0).ok());
    (*delay)->serviceMainThread();
    CHECK((*delay)->restartPending());
    CHECK((*delay)->active());
    (*delay)->stopProcessing();
    CHECK((*delay)->serviceMainThread());
    CHECK_FALSE((*delay)->restartPending());
    CHECK((*delay)->active());
    CHECK((*delay)->latency() == 0);
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

TEST_CASE("a VST3's edit controller is reached from the main thread only") {
    // A VST3 parameter has two domains: the plain value the commands use and
    // the normalized value the processor is given with its events. The engine
    // converts before the audio thread (paramsNormalized() tells it to), and
    // the conversions themselves are UI-thread calls per the SDK — this counts
    // any that run on another thread while the plug-in is processing.
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    const auto gain = std::find_if(found->begin(), found->end(),
                                   [](const PluginInfo& p) { return p.name == "Test VST3 Gain"; });
    REQUIRE(gain != found->end());
    const ControllerWatch watch(std::filesystem::path(FIXTURE_VST3_MODULE));
    REQUIRE(watch.ok());
    watch.reset();
    auto made = Instance::create(*gain, {});
    REQUIRE(made);
    auto& instance = **made;
    CHECK(instance.paramsNormalized());  // and the engine converts for it
    CHECK(*instance.normalizedParam(1, 0.25) == doctest::Approx(0.25));
    CHECK(*instance.plainParam(1, 0.75) == doctest::Approx(0.75));
    REQUIRE(instance.activate(48000, 1, 64).ok());
    REQUIRE(instance.startProcessing());
    Buffers buffer(64);
    std::fill(buffer.l.begin(), buffer.l.end(), 1.0f);
    std::fill(buffer.r.begin(), buffer.r.end(), 1.0f);
    REQUIRE(instance.setParam(1, 0.25).ok());  // converted here, not on the audio thread
    Event automation{.kind = Event::Kind::Param, .time = 10, .paramId = 1, .value = 0.75};
    REQUIRE(instance.process(buffer.block(64, std::span<const Event>(&automation, 1))));
    CHECK(static_cast<double>(buffer.ol[9]) == doctest::Approx(0.25));   // queued first
    CHECK(static_cast<double>(buffer.ol[10]) == doctest::Approx(0.75));  // then the lane
    CHECK(watch.count() == 0);
    // The watch itself: a second thread in the controller while processing is
    // what it counts (calling the controller concurrently is the caller's bug).
    std::thread other([&] { (void)instance.paramValue(1); });
    other.join();
    CHECK(watch.count() >= 1);
    instance.stopProcessing();
    instance.deactivate();
    // CLAP has a single domain: the conversions are the identity there.
    auto clapGain = find("com.suppleaudio.test.gain");
    auto plain = Instance::create(clapGain, {});
    REQUIRE(plain);
    CHECK_FALSE((*plain)->paramsNormalized());
    CHECK(*(*plain)->normalizedParam(1, 0.4) == doctest::Approx(0.4));
    CHECK(*(*plain)->plainParam(1, 0.4) == doctest::Approx(0.4));
}

TEST_CASE("a VST3 reset ends the notes the host delivered") {
    // A VST3 has no reset of its own: the host remembers the notes it sent and
    // releases them at the next process (the engine relies on that for "no
    // hanging notes" when an instance leaves an executor).
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    const auto sine = std::find_if(found->begin(), found->end(),
                                   [](const PluginInfo& p) { return p.name == "Test VST3 Sine"; });
    REQUIRE(sine != found->end());
    auto made = Instance::create(*sine, {});
    REQUIRE(made);
    auto& instrument = **made;
    REQUIRE(instrument.activate(48000, 1, 64).ok());
    REQUIRE(instrument.startProcessing());
    Buffers buffer(64);
    Event note{.kind = Event::Kind::NoteOn, .time = 0, .key = 69, .noteId = 3};
    REQUIRE(instrument.process(buffer.block(64, std::span<const Event>(&note, 1), false)));
    CHECK(std::abs(buffer.ol[32]) > 0.01f);  // the voice is sounding
    instrument.reset();
    buffer = Buffers(64);
    REQUIRE(instrument.process(buffer.block(64, {}, false)));
    CHECK(buffer.ol[32] == 0.0f);  // the reset note-off ended it
    instrument.stopProcessing();
    instrument.deactivate();
}

TEST_CASE("scan finds the fixtures with their capabilities") {
    auto list = scan(std::span<const std::filesystem::path>(&kFixture, 1));
    REQUIRE(list);
    REQUIRE(list->size() == 16);
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

TEST_CASE("a multi-out instrument's auxiliary outputs reach the block, in process and bridged") {
    setBridgeExecutable(BRIDGE_PATH);
    for (bool isolated : {false, true}) {
        CAPTURE(isolated);
        auto made = Instance::create(find("com.suppleaudio.test.multiout"), {}, isolated);
        REQUIRE(made);
        Instance& p = **made;
        CHECK(p.audioInputChannels() == 0);
        CHECK(p.audioOutputChannels() == 2);
        REQUIRE(p.auxOutputPorts().size() == 2);
        CHECK(p.auxOutputPorts()[0].name == "Kick");
        CHECK(p.auxOutputPorts()[1].name == "Snare");
        CHECK(p.auxOutputPorts()[1].channels == 2);
        CHECK(p.auxOutputChannels() == 4);
        REQUIRE(p.activate(48000, 1, 64));
        REQUIRE(p.startProcessing());
        Buffers buf(32);
        std::vector<std::vector<float>> aux(3, std::vector<float>(32, -1.0f));
        float* auxOut[3] = {aux[0].data(), aux[1].data(), aux[2].data()};
        ProcessBlock b = buf.block(32, {}, false);
        b.auxOutputs = auxOut;
        b.auxOutputChannels = 3;  // one short: the Snare's right goes to scratch
        REQUIRE(p.process(b));
        CHECK(buf.ol[31] == 0.125f);
        CHECK(aux[0][0] == 0.25f);
        CHECK(aux[1][31] == 0.25f);
        CHECK(aux[2][7] == 0.5f);
        // Without aux buffers the plug-in still gets every port (scratch).
        REQUIRE(p.process(buf.block(32, {}, false)));
        CHECK(buf.orr[0] == 0.125f);
        p.stopProcessing();
        p.deactivate();
    }
}

TEST_CASE("a 49-port instrument loads with all its outputs, in process and bridged") {
    // LSP's x48 DirectOut multi-sampler has 49 output ports.
    setBridgeExecutable(BRIDGE_PATH);
    for (bool isolated : {false, true}) {
        CAPTURE(isolated);
        auto made = Instance::create(find("com.suppleaudio.test.multiout-x48"), {}, isolated);
        REQUIRE_MESSAGE(made, (made ? "" : made.error().toString()));
        Instance& p = **made;
        REQUIRE(p.auxOutputPorts().size() == 48);
        CHECK(p.auxOutputPorts()[47].name == "Out 49");
        CHECK(p.auxOutputChannels() == 96);
        REQUIRE(p.activate(48000, 1, 64));
        REQUIRE(p.startProcessing());
        Buffers buf(16);
        std::vector<std::vector<float>> aux(96, std::vector<float>(16, -1.0f));
        std::vector<float*> ptrs;
        for (auto& a : aux) ptrs.push_back(a.data());
        ProcessBlock b = buf.block(16, {}, false);
        b.auxOutputs = ptrs.data();
        b.auxOutputChannels = 96;
        REQUIRE(p.process(b));
        CHECK(buf.ol[0] == 0.125f);
        CHECK(aux[0][0] == 0.25f);
        CHECK(aux[95][15] == 0.5f);
        p.stopProcessing();
        p.deactivate();
    }
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
    CHECK(out.storage[0].port == 0);
    CHECK(out.storage[0].channel == 0);
    CHECK(out.storage[0].key == 69);
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

TEST_CASE("undersized CLAP output events are rejected locally and bridged") {
    setBridgeExecutable(BRIDGE_PATH);
    for (bool isolated : {false, true}) {
        auto made = Instance::create(find("com.suppleaudio.test.edge"), {}, isolated);
        REQUIRE(made);
        const std::array<std::uint8_t, 4> config{40, 0, 0, 0};
        REQUIRE((*made)->loadState(config));
        REQUIRE((*made)->activate(48000, 1, 64));
        REQUIRE((*made)->startProcessing());
        Buffers buffer(64);
        std::array<OutputEvent, 8> storage{};
        OutputEvents sink{storage};
        REQUIRE((*made)->process(buffer.block(64), &sink));
        REQUIRE(sink.count == 1);
        CHECK(storage[0].kind == OutputEvent::Kind::ParamValue);
        CHECK(storage[0].paramId == 7);
        CHECK(storage[0].value == 42);
        (*made)->stopProcessing();
        (*made)->deactivate();
    }
}

TEST_CASE("process outputs retain flags and anonymous note addresses locally and bridged") {
    setBridgeExecutable(BRIDGE_PATH);
    for (bool isolated : {false, true}) {
        CAPTURE(isolated);
        auto made = Instance::create(find("com.suppleaudio.test.edge"), {}, isolated);
        REQUIRE(made);
        auto& p = **made;
        REQUIRE(p.activate(48000, 1, 64));
        REQUIRE(p.startProcessing());
        Buffers buffers(64);
        const Event notes[] = {{.kind = Event::Kind::NoteOn,
                                .time = 3,
                                .port = 0,
                                .channel = 3,
                                .key = 64,
                                .noteId = -1},
                               {.kind = Event::Kind::NoteOff,
                                .time = 11,
                                .port = 0,
                                .channel = 3,
                                .key = 64,
                                .noteId = -1}};
        std::array<OutputEvent, 2> storage;
        OutputEvents out{storage};
        REQUIRE(rtBlock(p, buffers.block(64, notes), out));
        REQUIRE(out.count == 2);
        CHECK(out.dropped == 0);
        const auto& value = storage[0];
        CHECK(value.kind == OutputEvent::Kind::ParamValue);
        CHECK(value.flags == (OutputEvent::DontRecord | OutputEvent::IsLive));
        CHECK(value.paramId == 7);
        CHECK(value.value == 42);
        CHECK(value.noteId == -1);
        CHECK(value.port == -1);
        CHECK(value.channel == -1);
        CHECK(value.key == -1);
        const auto& end = storage[1];
        CHECK(end.kind == OutputEvent::Kind::NoteEnd);
        CHECK(end.time == 11);
        CHECK(end.noteId == -1);
        CHECK(end.port == 0);
        CHECK(end.channel == 3);
        CHECK(end.key == 64);
        CHECK(end.flags == 0);
        out.clear();
        out.storage = std::span<OutputEvent>(storage.data(), 1);
        REQUIRE(rtBlock(p, buffers.block(64, notes), out));
        CHECK(out.count == 1);
        CHECK(out.dropped == 1);
        CHECK(p.takeIdleOutputs().events.empty());
        p.stopProcessing();
        p.deactivate();
    }
}

TEST_CASE("VST3 parameter outputs retain normalized values and default metadata") {
    setBridgeExecutable(BRIDGE_PATH);
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    auto gain = std::find_if(found->begin(), found->end(),
                             [](const PluginInfo& p) { return p.name == "Test VST3 Gain"; });
    REQUIRE(gain != found->end());
    for (bool isolated : {false, true}) {
        CAPTURE(isolated);
        auto made = Instance::create(*gain, {}, isolated);
        REQUIRE(made);
        auto& p = **made;
        REQUIRE(p.activate(48000, 1, 64));
        REQUIRE(p.startProcessing());
        const Event param{.kind = Event::Kind::Param, .time = 7, .paramId = 1, .value = 0.625};
        Buffers buffers(64);
        std::array<OutputEvent, 1> storage;
        OutputEvents out{storage};
        REQUIRE(rtBlock(p, buffers.block(64, std::span<const Event>(&param, 1)), out));
        REQUIRE(out.count == 1);
        CHECK(out.dropped == 0);
        const auto& value = storage[0];
        CHECK(value.kind == OutputEvent::Kind::ParamValue);
        CHECK(value.time == 7);
        CHECK(value.paramId == 1);
        CHECK(value.value == 0.625);
        CHECK(value.flags == 0);
        CHECK(value.noteId == -1);
        CHECK(value.port == -1);
        CHECK(value.channel == -1);
        CHECK(value.key == -1);
        CHECK(p.paramsNormalized());
        p.stopProcessing();
        p.deactivate();
    }
}

namespace {
void checkIdleGesture(const Instance::IdleOutputs& out, double value) {
    REQUIRE(out.events.size() == 3);
    CHECK(out.dropped == 0);
    CHECK(out.events[0].kind == OutputEvent::Kind::GestureBegin);
    CHECK(out.events[1].kind == OutputEvent::Kind::ParamValue);
    CHECK(out.events[2].kind == OutputEvent::Kind::GestureEnd);
    CHECK(out.events[1].value == value);
    for (const auto& e : out.events) {
        CHECK(e.paramId == 7);
        CHECK(e.time == 0);
        CHECK((e.flags & OutputEvent::IsLive) != 0);
        CHECK(e.noteId == -1);
        CHECK(e.port == -1);
        CHECK(e.channel == -1);
        CHECK(e.key == -1);
    }
    CHECK((out.events[1].flags & OutputEvent::DontRecord) != 0);
}
}  // namespace

TEST_CASE("stopped editor flush outputs survive locally and bridged, on both flush threads") {
    setBridgeExecutable(BRIDGE_PATH);
    for (bool isolated : {false, true}) {
        CAPTURE(isolated);
        auto made = Instance::create(find("com.suppleaudio.test.edge"), {}, isolated);
        REQUIRE(made);
        auto& p = **made;
        const std::uint8_t state[4] = {9, 0, 0, 0};
        REQUIRE(p.loadState(state));
        p.serviceMainThread();
        checkIdleGesture(p.takeIdleOutputs(), 1);
        REQUIRE(p.openEditor(0x5a17, 1.0));  // fixture editor uses no display/window
        p.serviceMainThread();
        checkIdleGesture(p.takeIdleOutputs(), 2);
        CHECK(p.takeIdleOutputs().events.empty());
        REQUIRE(p.activate(48000, 1, 64));
        REQUIRE(p.loadState(state));
        p.serviceMainThread();
        REQUIRE(p.needsAudioFlush());
        std::thread audio([&] { p.flushParams(); });
        audio.join();
        checkIdleGesture(p.takeIdleOutputs(), 3);
        p.deactivate();
        REQUIRE(p.setParam(7, 0));
        checkIdleGesture(p.takeIdleOutputs(), 4);
        p.closeEditor();
        const auto empty = p.takeIdleOutputs();
        CHECK(empty.events.empty());
        CHECK(empty.dropped == 0);
    }
}

TEST_CASE("idle output overflow is counted and draining reopens capacity") {
    setBridgeExecutable(BRIDGE_PATH);
    for (bool isolated : {false, true}) {
        for (bool active : {false, true}) {
            CAPTURE(isolated);
            CAPTURE(active);
            auto made = Instance::create(find("com.suppleaudio.test.edge"), {}, isolated);
            REQUIRE(made);
            auto& p = **made;
            if (active) REQUIRE(p.activate(48000, 1, 64));
            auto flush = [&](std::uint8_t mode) {
                const std::uint8_t state[4] = {mode, 0, 0, 0};
                REQUIRE(p.loadState(state));
                p.serviceMainThread();
                if (active) {
                    std::thread audio([&] { p.flushParams(); });
                    audio.join();
                }
            };
            flush(10);
            flush(10);
            const auto out = p.takeIdleOutputs();
            REQUIRE(out.events.size() == Instance::IdleOutputs::kCapacity);
            CHECK(out.dropped == 3600 - Instance::IdleOutputs::kCapacity);
            for (std::size_t i = 0; i < out.events.size(); ++i) {
                const OutputEvent::Kind kinds[] = {OutputEvent::Kind::GestureBegin,
                                                   OutputEvent::Kind::ParamValue,
                                                   OutputEvent::Kind::GestureEnd};
                CHECK(out.events[i].kind == kinds[i % 3]);
            }
            const auto empty = p.takeIdleOutputs();
            CHECK(empty.events.empty());
            CHECK(empty.dropped == 0);
            flush(9);
            checkIdleGesture(p.takeIdleOutputs(), 3);
            if (active) p.deactivate();
        }
    }
}

TEST_CASE("bridge idle output codec rejects malformed replies and preserves metadata") {
    using namespace bridge;
    const OutputEvent event{.kind = OutputEvent::Kind::NoteEnd,
                            .time = 12,
                            .noteId = -1,
                            .flags = OutputEvent::DontRecord,
                            .port = 2,
                            .channel = 4,
                            .key = 65};
    Writer w;
    w.u64(17);
    w.u32(1);
    writeOutputEvent(w, event);
    REQUIRE(validReply(Op::TakeIdleOutputs, Reader(w.bytes.data(), w.bytes.size())));
    Reader r(w.bytes.data(), w.bytes.size());
    CHECK(r.u64() == 17);
    CHECK(r.u32() == 1);
    const auto decoded = readOutputEvent(r);
    CHECK(r.done());
    CHECK(decoded.flags == event.flags);
    CHECK(decoded.noteId == -1);
    CHECK(decoded.port == 2);
    CHECK(decoded.channel == 4);
    CHECK(decoded.key == 65);
    w.bytes.pop_back();
    CHECK_FALSE(validReply(Op::TakeIdleOutputs, Reader(w.bytes.data(), w.bytes.size())));
    Writer excessive;
    excessive.u64(0);
    excessive.u32(Instance::IdleOutputs::kCapacity + 1);
    CHECK_FALSE(
        validReply(Op::TakeIdleOutputs, Reader(excessive.bytes.data(), excessive.bytes.size())));
    Writer invalid;
    invalid.u64(0);
    invalid.u32(1);
    auto badKind = event;
    badKind.kind = static_cast<OutputEvent::Kind>(255);
    writeOutputEvent(invalid, badKind);
    CHECK_FALSE(
        validReply(Op::TakeIdleOutputs, Reader(invalid.bytes.data(), invalid.bytes.size())));
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
    CHECK(g.restartPending());  // the engine takes it off its executors for this
    g.deactivate();             // refused while processing
    CHECK(g.active());
    bool logged = false;
    for (const auto& line : g.takeLog())
        logged = logged || line.find("deactivate") != std::string::npos;
    CHECK(logged);
    g.stopProcessing();
    CHECK(g.serviceMainThread());
    CHECK(g.active());
    CHECK(g.latency() == 64);
    CHECK_FALSE(g.restartPending());
}

TEST_CASE("lifecycle: a bridged plug-in's restart waits for processing too, and says so") {
    setBridgeExecutable(BRIDGE_PATH);
    auto inst = Instance::create(find("com.suppleaudio.test.gain"), {}, true);
    REQUIRE_MESSAGE(inst, (inst ? "" : inst.error().message));
    auto& g = **inst;
    REQUIRE(g.activate(48000, 1, 256).ok());
    REQUIRE(g.startProcessing());
    Buffers buf(64);
    Event e{.kind = Event::Kind::Param, .paramId = 1, .value = 1.0};  // latency "short"
    REQUIRE(g.process(buf.block(64, std::span<const Event>(&e, 1))));
    g.serviceMainThread();
    CHECK(g.restartPending());
    CHECK(g.latency() == 0);
    g.stopProcessing();
    for (int i = 0; i < 10 && g.latency() != 64; ++i) g.serviceMainThread();
    CHECK(g.latency() == 64);
    CHECK_FALSE(g.restartPending());
    g.deactivate();
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

TEST_CASE("an editor opens in process and through the bridge; a dead bridge closes it (gap #39)") {
    setBridgeExecutable(BRIDGE_PATH);
    for (bool isolated : {false, true}) {
        CAPTURE(isolated);
        auto made = Instance::create(find("com.suppleaudio.test.edge"), {}, isolated);
        REQUIRE(made);
        Instance& p = **made;
        REQUIRE(p.hasEditor());
        // The parent window id reaches the plug-in (across the process boundary).
        auto size = p.openEditor(0x5a17, 1.0);
        REQUIRE_MESSAGE(size, (size ? "" : size.error().toString()));
        CHECK(size->width == 300);
        CHECK(size->height == 200);
        CHECK(size->resizable);
        CHECK(p.editorOpen());
        CHECK(p.paramValue(8).value() == 0x5a17);
        // The resize it asked for when shown arrives (polled through the bridge).
        Instance::EditorRequests q;
        while (!q.resize) {
            q = p.takeEditorRequests();
            if (!q.resize) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(q.resize);
        CHECK(q.width == 320);
        CHECK(q.height == 240);
        CHECK(p.nextTimerMs() >= -1);
        // A host-driven resize is adjusted by the plug-in.
        auto resized = p.resizeEditor(50, 400);
        REQUIRE(resized);
        CHECK(resized->width == 100);
        CHECK(resized->height == 400);
        p.closeEditor();
        CHECK_FALSE(p.editorOpen());
        CHECK(p.paramValue(8).value() == 0);
        if (!isolated) continue;
        // The bridge dies with the editor open: the next poll reports it closed.
        REQUIRE(p.openEditor(0x5a18, 1.0));
        REQUIRE(p.bridgePid() > 0);
        ::kill(p.bridgePid(), SIGKILL);
        q = {};
        while (!q.closed) {
            q = p.takeEditorRequests();
            if (!q.closed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(q.closed);
        CHECK_FALSE(p.editorOpen());
        CHECK_FALSE(p.hasEditor());
    }
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

namespace {
// The threads of process `pid` with name `name` (by /proc/<pid>/task/<tid>/comm).
std::vector<pid_t> tasksNamed(int pid, std::string_view name) {
    std::vector<pid_t> tasks;
    std::error_code ec;
    const auto dir = std::filesystem::path("/proc") / std::to_string(pid) / "task";
    for (const auto& task : std::filesystem::directory_iterator(dir, ec)) {
        std::ifstream comm(task.path() / "comm");
        std::string text;
        if (std::getline(comm, text) && text == name)
            tasks.push_back(static_cast<pid_t>(std::stol(task.path().filename().string())));
    }
    return tasks;
}
template <class Predicate>
bool eventually(Predicate predicate) {  // the deadline only bounds a failure
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return predicate();
}
}  // namespace

TEST_CASE("a bridged instance's audio thread is named audio-bridge") {
    setBridgeExecutable(BRIDGE_PATH);
    auto remote = Instance::create(find("com.suppleaudio.test.gain"), {}, true);
    REQUIRE_MESSAGE(remote, (remote ? "" : remote.error().toString()));
    Instance& r = **remote;
    REQUIRE(r.activate(48000, 1, 256).ok());
    CHECK(eventually([&] { return tasksNamed(r.bridgePid(), "audio-bridge").size() == 1; }));
    r.deactivate();
}

TEST_CASE("a bridged audio thread takes the bridge audio policy when processing starts") {
    using Kind = ThreadPolicy::Kind;
    const ThreadPolicy normal{Kind::Normal, 0};
    auto policyOf = [](pid_t tid) {
        sched_param params{};
        const int policy = ::sched_getscheduler(tid);
        if (policy < 0 || ::sched_getparam(tid, &params) != 0) return ThreadPolicy{};
        switch (policy & ~SCHED_RESET_ON_FORK) {
            case SCHED_FIFO:
                return ThreadPolicy{Kind::Fifo, params.sched_priority};
            case SCHED_RR:
                return ThreadPolicy{Kind::RoundRobin, params.sched_priority};
            default:
                return ThreadPolicy{Kind::Normal, 0};
        }
    };
    rlimit rtprio{}, rttime{};
    const bool realtime = ::getrlimit(RLIMIT_RTPRIO, &rtprio) == 0 && rtprio.rlim_cur >= 1 &&
                          ::getrlimit(RLIMIT_RTTIME, &rttime) == 0 &&
                          rttime.rlim_cur == RLIM_INFINITY;
    setBridgeExecutable(BRIDGE_PATH);
    auto remote = Instance::create(find("com.suppleaudio.test.gain"), {}, true);
    REQUIRE_MESSAGE(remote, (remote ? "" : remote.error().toString()));
    Instance& r = **remote;
    REQUIRE(r.activate(48000, 1, 256).ok());
    REQUIRE(eventually([&] { return tasksNamed(r.bridgePid(), "audio-bridge").size() == 1; }));
    const pid_t audio = tasksNamed(r.bridgePid(), "audio-bridge").front();
    CHECK(policyOf(audio) == normal);
    const ThreadPolicy fifo{Kind::Fifo, 1};
    setBridgeAudioPolicy(realtime ? fifo : normal);
    REQUIRE(r.startProcessing());  // applied on the audio thread before it replies
    CHECK(policyOf(audio) == (realtime ? fifo : normal));
    r.stopProcessing();
    setBridgeAudioPolicy(normal);
    REQUIRE(r.startProcessing());
    CHECK(policyOf(audio) == normal);
    r.stopProcessing();
    CHECK_FALSE(r.failed());
    r.deactivate();
    if (!realtime) MESSAGE("no real-time scheduling permission: only the normal policy checked");
}

TEST_CASE("shared bridge routes two instances through one process and fails both on a crash") {
    setBridgeExecutable(BRIDGE_PATH);
    auto process = createBridgeProcess();
    REQUIRE(process);
    const auto gain = find("com.suppleaudio.test.gain");
    const auto crash = find("com.suppleaudio.test.crash");
    auto first = Instance::create(gain, {}, true, *process);
    auto second = Instance::create(crash, {}, true, *process);
    auto local = Instance::create(gain, {});
    auto dedicated = Instance::create(gain, {}, true);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(local);
    REQUIRE(dedicated);
    CHECK((*first)->bridgePid() > 0);
    CHECK((*first)->bridgePid() == (*second)->bridgePid());
    CHECK((*first)->bridgePid() != (*dedicated)->bridgePid());
    auto transient = Instance::create(gain, {}, true, *process);
    REQUIRE(transient);
    CHECK((*transient)->bridgePid() == (*first)->bridgePid());
    (*transient).reset();  // Destroy removes only this instance.
    REQUIRE((*second)->paramValue(1));

    for (Instance* instance : {first->get(), second->get(), local->get(), dedicated->get()}) {
        REQUIRE(instance->activate(48000, 1, 256));
        REQUIRE(instance->startProcessing());
    }
    Buffers a(256), b(256), l(256), d(256);
    for (std::size_t i = 0; i < a.l.size(); ++i) {
        const float sample = std::sin(0.02f * static_cast<float>(i));
        a.l[i] = a.r[i] = b.l[i] = b.r[i] = l.l[i] = l.r[i] = d.l[i] = d.r[i] = sample;
    }
    REQUIRE((*first)->process(a.block(256)));
    REQUIRE((*second)->process(b.block(256)));
    REQUIRE((*local)->process(l.block(256)));
    REQUIRE((*dedicated)->process(d.block(256)));
    CHECK(a.ol == l.ol);
    CHECK(a.orr == l.orr);
    CHECK(b.ol == l.ol);  // the crash fixture passes audio through until triggered
    CHECK(b.orr == l.orr);
    CHECK(d.ol == l.ol);

    REQUIRE((*second)->setParam(1, 1.0));
    REQUIRE((*second)->process(b.block(256)));  // crash is isolated
    CHECK((*first)->failed());
    CHECK((*second)->failed());
    CHECK_FALSE((*dedicated)->failed());
    REQUIRE((*first)->process(a.block(256)));
    CHECK(std::all_of(a.ol.begin(), a.ol.end(), [](float v) { return v == 0.0f; }));
    REQUIRE((*dedicated)->process(d.block(256)));
    CHECK(d.ol == l.ol);
    for (Instance* instance : {first->get(), second->get(), local->get(), dedicated->get()}) {
        instance->stopProcessing();
        instance->deactivate();
    }
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

TEST_CASE("a sidechain key reaches CLAP and VST3 plug-ins, in process and isolated (ADR-0018)") {
    // Every declared port gets a buffer (the key-mix fixture errors otherwise);
    // the key sums into the output; without a key the plug-in hears silence.
    auto check = [](Instance& inst, bool keyActive) {
        REQUIRE(inst.sidechainChannels() == 2);
        REQUIRE(inst.setSidechainActive(keyActive).ok());
        REQUIRE(inst.activate(48000, 1, 64).ok());
        REQUIRE(inst.startProcessing());
        Buffers buffer(64);
        std::fill(buffer.l.begin(), buffer.l.end(), 0.25f);
        std::fill(buffer.r.begin(), buffer.r.end(), 0.25f);
        std::vector<float> kl(64, 0.5f), kr(64, -0.5f);
        const float* key[2] = {kl.data(), kr.data()};
        ProcessBlock b = buffer.block(64);
        b.sidechain = key;
        b.sidechainChannels = 2;
        REQUIRE(inst.process(b));
        const double withKeyL = static_cast<double>(buffer.ol[7]),
                     withKeyR = static_cast<double>(buffer.orr[7]);
        REQUIRE(inst.process(buffer.block(64)));  // no key: silence
        const double withoutL = static_cast<double>(buffer.ol[7]);
        inst.stopProcessing();
        inst.deactivate();
        return std::array<double, 3>{withKeyL, withKeyR, withoutL};
    };
    auto clap = scan(std::span<const std::filesystem::path>(&kFixture, 1));
    REQUIRE(clap);
    const auto keyMix = std::find_if(clap->begin(), clap->end(), [](const PluginInfo& p) {
        return p.id == "com.suppleaudio.test.keymix";
    });
    REQUIRE(keyMix != clap->end());
    for (bool isolated : {false, true}) {
        CAPTURE(isolated);
        auto made = Instance::create(*keyMix, {}, isolated);
        REQUIRE(made);
        const auto r = check(**made, true);
        CHECK(r[0] == doctest::Approx(0.75));
        CHECK(r[1] == doctest::Approx(-0.25));
        CHECK(r[2] == doctest::Approx(0.25));
    }
    auto vst3 = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(vst3);
    const auto sidechain = std::find_if(vst3->begin(), vst3->end(), [](const PluginInfo& p) {
        return p.name == "Test VST3 Sidechain Gain";
    });
    REQUIRE(sidechain != vst3->end());
    for (bool isolated : {false, true})
        for (bool active : {true, false}) {
            CAPTURE(isolated);
            CAPTURE(active);
            auto made = Instance::create(*sidechain, {}, isolated);
            REQUIRE(made);
            REQUIRE((*made)->setParam(1, 1.0).ok());
            const auto r = check(**made, active);
            // An inactive key bus hears nothing, as when no route feeds it.
            CHECK(r[0] == doctest::Approx(active ? 0.75 : 0.25));
            CHECK(r[2] == doctest::Approx(0.25));
        }
}

TEST_CASE("a plugin found in two places resolves to the first on the search path") {
    // The same id in an explicit path and in ~/.clap (an older install): the
    // explicit one must win however the paths sort (they used to sort by name).
    namespace fs = std::filesystem;
    const fs::path base =
        fs::temp_directory_path() / ("supple-shadow-" + std::to_string(::getpid()));
    const fs::path first = base / "zz-explicit", second = base / "aa-installed";
    fs::create_directories(first);
    fs::create_directories(second);
    fs::copy_file(FIXTURE_CLAP_PATH, first / "fixture.clap", fs::copy_options::overwrite_existing);
    fs::copy_file(FIXTURE_CLAP_PATH, second / "fixture.clap", fs::copy_options::overwrite_existing);
    const std::vector<fs::path> path = {first, second};
    auto list = host::scan(path);
    REQUIRE(list);
    host::orderCatalogue(*list);
    std::optional<std::size_t> at;
    for (std::size_t i = 0; i < list->size(); ++i)
        if (!at && (*list)[i].path.parent_path() == first) at = i;
    REQUIRE(at);
    const std::string id = (*list)[*at].id;
    std::size_t firstOfId = list->size();
    for (std::size_t i = 0; i < list->size(); ++i)
        if ((*list)[i].id == id) {
            firstOfId = i;
            break;
        }
    CHECK((*list)[firstOfId].path.parent_path() == first);  // what a lookup by id takes
    CHECK_FALSE(host::shadowed(*list, firstOfId));
    REQUIRE(firstOfId + 1 < list->size());
    CHECK((*list)[firstOfId + 1].path.parent_path() == second);
    CHECK(host::shadowed(*list, firstOfId + 1));
    fs::remove_all(base);
}

TEST_CASE("CLAP timers may remove themselves and replace registrations") {
    auto made = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(made);
    const std::array<std::uint8_t, 4> config{2, 0, 0, 0};
    REQUIRE((*made)->loadState(config));
    REQUIRE((*made)->serviceMainThread());
    CHECK(*(*made)->paramValue(9) == 1);
    REQUIRE((*made)->serviceMainThread());
    CHECK(*(*made)->paramValue(9) == 2);
    CHECK((*made)->nextTimerMs() == -1);
}
TEST_CASE("activation and process reject invalid dimensions before touching buffers") {
    setBridgeExecutable(BRIDGE_PATH);
    for (bool isolated : {false, true}) {
        auto made = Instance::create(find("com.suppleaudio.test.edge"), {}, isolated);
        REQUIRE(made);
        auto& instance = **made;
        for (double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()})
            CHECK_FALSE(instance.activate(rate, 1, 64));
        CHECK_FALSE(instance.activate(48000, 0, 64));
        CHECK_FALSE(instance.activate(48000, 65, 64));
        if (isolated) CHECK_FALSE(instance.activate(48000, 1, UINT32_MAX));
        REQUIRE(instance.activate(48000, 1, 64));
        REQUIRE(instance.startProcessing());
        Buffers buffer(65);
        std::fill(buffer.ol.begin(), buffer.ol.end(), 123.0f);
        CHECK_FALSE(instance.process(buffer.block(65)));
        CHECK_FALSE(instance.process(buffer.block(0)));
        CHECK(buffer.ol.front() == 123.0f);
        CHECK(buffer.ol.back() == 123.0f);
        CHECK(instance.process(buffer.block(64)));
        instance.stopProcessing();
        instance.deactivate();
    }
}
TEST_CASE("VST3 queue rejection preserves controller value and malformed MIDI is dropped") {
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    auto gain = std::find_if(found->begin(), found->end(),
                             [](const auto& p) { return p.name == "Test VST3 Gain"; });
    REQUIRE(gain != found->end());
    auto made = Instance::create(*gain, {});
    REQUIRE(made);
    auto& instance = **made;
    CHECK_FALSE(instance.activate(48000, 1, UINT32_MAX));
    bool full = false;
    for (int i = 0; i < 4096; ++i) {
        if (!instance.setParam(1, 0.25)) {
            full = true;
            break;
        }
    }
    REQUIRE(full);
    CHECK_FALSE(instance.setParam(1, 0.75));
    CHECK(*instance.paramValue(1) == doctest::Approx(0.25));
    REQUIRE(instance.activate(48000, 1, 64));
    REQUIRE(instance.startProcessing());
    Buffers buffer(64);
    std::array<Event, 128> events{};
    for (std::size_t i = 0; i < events.size(); ++i) {
        events[i].kind = Event::Kind::Midi;
        events[i].midi[0] = 0xb0;
        events[i].midi[1] = static_cast<std::uint8_t>(128 + i);
        events[i].midi[2] = 127;
    }
    CHECK(instance.process(buffer.block(64, events)));
    CHECK(instance.takeNotifications().droppedEvents == events.size());
    instance.stopProcessing();
    instance.deactivate();
}
TEST_CASE("VST3 scan retains search path precedence and suppresses repeated bundles") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("supple-vst3-order-" + std::to_string(getpid()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{root};
    const auto first = root / "z-first" / "Fixture.vst3";
    const auto second = root / "a-second" / "Fixture.vst3";
    for (const auto& bundle : {first, second}) {
        fs::create_directories(bundle / "Contents/Resources");
        fs::copy_file(kVst3Fixture / "Contents/Resources/moduleinfo.json",
                      bundle / "Contents/Resources/moduleinfo.json",
                      fs::copy_options::overwrite_existing);
    }
    const std::array<fs::path, 3> paths{first.parent_path(), second.parent_path(), first};
    auto found = scan(paths);
    REQUIRE(found);
    REQUIRE(found->size() == 14);  // two bundles of seven classes
    CHECK(found->front().path == first);
    CHECK(found->back().path == second);
}
TEST_CASE("bridge rejected frames close received descriptors") {
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    int pipes[2];
    REQUIRE(::pipe(pipes) == 0);
    REQUIRE(::fcntl(pipes[0], F_SETFL, O_NONBLOCK) == 0);
    struct Cleanup {
        int* sockets;
        int* pipes;
        ~Cleanup() {
            for (int i = 0; i < 2; ++i) {
                ::close(sockets[i]);
                ::close(pipes[i]);
            }
        }
    } cleanup{sockets, pipes};
    const std::uint32_t prefix[3] = {bridge::kMaxMessage + 1, 0, 0};
    iovec iov{const_cast<std::uint32_t*>(prefix), sizeof(prefix)};
    alignas(cmsghdr) char control[CMSG_SPACE(2 * sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    auto* c = CMSG_FIRSTHDR(&message);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(2 * sizeof(int));
    const int descriptors[2] = {pipes[1], pipes[1]};
    std::memcpy(CMSG_DATA(c), descriptors, sizeof(descriptors));
    REQUIRE(::sendmsg(sockets[0], &message, MSG_NOSIGNAL) == sizeof(prefix));
    std::uint32_t head = 0, id = 0;
    std::vector<std::uint8_t> payload;
    int received = -1;
    CHECK_FALSE(bridge::recvFrame(sockets[1], head, id, payload, &received, 100));
    CHECK(received == -1);
    ::close(pipes[1]);
    pipes[1] = -1;
    char byte;
    CHECK(::read(pipes[0], &byte, 1) == 0);
}

TEST_CASE("bridge rejects undersized shared memory and unexpected descriptors without dying") {
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    posix_spawn_file_actions_t actions;
    REQUIRE(posix_spawn_file_actions_init(&actions) == 0);
    REQUIRE(posix_spawn_file_actions_adddup2(&actions, sockets[1], bridge::kControlFd) == 0);
    std::string executable = BRIDGE_PATH;
    std::string descriptor = std::to_string(bridge::kControlFd);
    char controlArg[] = "--control-fd";
    char* argv[] = {executable.data(), controlArg, descriptor.data(), nullptr};
    pid_t pid = 0;
    const int spawned = ::posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(sockets[1]);
    REQUIRE(spawned == 0);
    struct Cleanup {
        int socket;
        pid_t pid;
        ~Cleanup() {
            ::close(socket);
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    } cleanup{sockets[0], pid};
    bridge::Writer create;
    create.str(kFixture.string());
    create.str("com.suppleaudio.test.edge");
    create.str("clap");
    for (int i = 0; i < 4; ++i) create.str("");
    REQUIRE(bridge::sendFrame(sockets[0], static_cast<std::uint32_t>(bridge::Op::Create), 0,
                              create.bytes));
    std::uint32_t status = 0, id = 0;
    std::vector<std::uint8_t> reply;
    REQUIRE(bridge::recvFrame(sockets[0], status, id, reply, nullptr, 2000));
    REQUIRE(status == static_cast<std::uint32_t>(bridge::Reply::Ok));
    bridge::Reader reader(reply.data(), reply.size());
    const auto instance = reader.u32();
    REQUIRE(bridge::sendFrame(sockets[0], static_cast<std::uint32_t>(bridge::Op::SetParam),
                              instance, {}));
    REQUIRE(bridge::recvFrame(sockets[0], status, id, reply, nullptr, 2000));
    REQUIRE(status == static_cast<std::uint32_t>(bridge::Reply::Error));
    bridge::Reader malformed(reply.data(), reply.size());
    CHECK(malformed.error().code == "bridge.malformed");
    const int memory =
        static_cast<int>(::syscall(SYS_memfd_create, "supple-test-shm", MFD_CLOEXEC));
    REQUIRE(memory >= 0);
    struct Memory {
        int fd;
        ~Memory() { ::close(fd); }
    } memoryCleanup{memory};
    bridge::Writer activate;
    activate.f64(48000);
    activate.u32(1);
    activate.u32(64);
    activate.u32(sizeof(bridge::Header));
    REQUIRE(bridge::sendFrame(sockets[0], static_cast<std::uint32_t>(bridge::Op::Activate),
                              instance, activate.bytes, memory));
    REQUIRE(bridge::recvFrame(sockets[0], status, id, reply, nullptr, 2000));
    CHECK(status == static_cast<std::uint32_t>(bridge::Reply::Error));
    REQUIRE(bridge::sendFrame(sockets[0], static_cast<std::uint32_t>(bridge::Op::Params), instance,
                              {}, memory));
    REQUIRE(bridge::recvFrame(sockets[0], status, id, reply, nullptr, 2000));
    CHECK(status == static_cast<std::uint32_t>(bridge::Reply::Error));
    REQUIRE(bridge::sendFrame(sockets[0], static_cast<std::uint32_t>(bridge::Op::Params), instance,
                              {}));
    REQUIRE(bridge::recvFrame(sockets[0], status, id, reply, nullptr, 2000));
    CHECK(status == static_cast<std::uint32_t>(bridge::Reply::Ok));
}

TEST_CASE("invalid CLAP symlink paths return a structured error instead of throwing") {
    namespace fs = std::filesystem;
    const auto path =
        fs::temp_directory_path() / ("supple-clap-loop-" + std::to_string(getpid()) + ".clap");
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove(path); }
    } cleanup{path};
    fs::remove(path);
    fs::create_symlink(path.filename(), path);
    auto info = find("com.suppleaudio.test.edge");
    info.path = path;
    auto made = Instance::create(info, {});
    REQUIRE_FALSE(made);
    CHECK(made.error().code == "host.load");
}

TEST_CASE("state restore is a queued-edit barrier and inactive saves flush edits") {
    setBridgeExecutable(BRIDGE_PATH);
    for (bool vst3 : {false, true})
        for (bool isolated : {false, true}) {
            CAPTURE(isolated);
            CAPTURE(vst3);
            PluginInfo info;
            if (vst3) {
                auto catalogue = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
                REQUIRE(catalogue);
                const auto found =
                    std::find_if(catalogue->begin(), catalogue->end(),
                                 [](const auto& p) { return p.name == "Test VST3 Separate Gain"; });
                REQUIRE(found != catalogue->end());
                info = *found;
            } else
                info = find("com.suppleaudio.test.gain");
            auto made = Instance::create(info, {}, isolated);
            REQUIRE(made);
            auto& instance = **made;
            const auto id = vst3 ? 1u : 0u;
            const double original = vst3 ? 0.25 : gainClap(-6);
            const double edited = vst3 ? 0.75 : gainClap(0);
            REQUIRE(instance.setParam(id, original).ok());
            auto saved = instance.saveState();
            REQUIRE_MESSAGE(saved, (saved ? "" : saved.error().toString()));
            REQUIRE(instance.activate(48000, 1, 64).ok());
            REQUIRE(instance.setParam(id, edited).ok());
            REQUIRE(instance.loadState(*saved).ok());
            REQUIRE(instance.startProcessing());
            Buffers buffers(64);
            std::fill(buffers.l.begin(), buffers.l.end(), 1.0f);
            std::fill(buffers.r.begin(), buffers.r.end(), 1.0f);
            REQUIRE(instance.process(buffers.block(64)));
            CHECK(buffers.ol[0] == doctest::Approx(vst3 ? 0.25 : std::pow(10.0, -6.0 / 20.0)));
            instance.stopProcessing();
            REQUIRE(instance.setParam(id, edited).ok());
            instance.deactivate();
            auto flushed = instance.saveState();
            REQUIRE(flushed);
            auto restored = Instance::create(info, {}, isolated);
            REQUIRE(restored);
            REQUIRE((*restored)->loadState(*flushed).ok());
            REQUIRE((*restored)->activate(48000, 1, 64).ok());
            REQUIRE((*restored)->startProcessing());
            REQUIRE((*restored)->process(buffers.block(64)));
            CHECK(buffers.ol[0] == doctest::Approx(vst3 ? 0.75 : 1.0));
            (*restored)->stopProcessing();
            (*restored)->deactivate();
        }
}

TEST_CASE("bridge requests fail when a scripted child stops reading") {
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    const auto pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(sockets[0]);
        for (;;) ::pause();
    }
    ::close(sockets[1]);
    auto process = BridgeTestAccess::adopt(sockets[0], pid);
    bridge::Writer request;
    request.bytes.resize(1 << 20);
    const auto reply = process->call(bridge::Op::LoadState, 1, request, -1, 20);
    REQUIRE_FALSE(reply);
    CHECK(reply.error().code == "host.bridge_failed");
    CHECK(process->failed());
    CHECK(process->failure().find("stopped responding") != std::string::npos);
}

TEST_CASE("malformed scripted bridge replies fail the whole process") {
    for (const auto op : {bridge::Op::ParamValue, bridge::Op::Params, bridge::Op::SaveState,
                          bridge::Op::Service, bridge::Op::Create, bridge::Op::TakeIdleOutputs}) {
        for (bool errorReply : {false, true}) {
            CAPTURE(op);
            CAPTURE(errorReply);
            int sockets[2];
            REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
            auto process = BridgeTestAccess::adopt(sockets[0], -1);
            std::thread peer([&] {
                std::uint32_t head = 0, id = 0;
                std::vector<std::uint8_t> payload;
                if (bridge::recvFrame(sockets[1], head, id, payload))
                    bridge::sendFrame(sockets[1], errorReply ? 1 : 0, id, {0xff});
                ::close(sockets[1]);
            });
            auto reply = process->call(op, 1, bridge::Writer{});
            peer.join();
            REQUIRE_FALSE(reply);
            CHECK(reply.error().code == "bridge.malformed");
            CHECK(process->failed());
        }
    }
}

TEST_CASE("bridge protocol v6 retains nested structured error details") {
    Error original("outer", "outer message", "/value", "retry");
    original.details.emplace_back("inner", "inner message", "/id", "correct it");
    original.details[0].details.emplace_back("leaf", "leaf message");
    bridge::Writer writer;
    writer.error(original);
    bridge::Reader reader(writer.bytes.data(), writer.bytes.size());
    const auto decoded = reader.error();
    REQUIRE(reader.done());
    REQUIRE(decoded.details.size() == 1);
    CHECK(decoded.details[0].path == "/id");
    CHECK(decoded.details[0].hint == "correct it");
    REQUIRE(decoded.details[0].details.size() == 1);
    CHECK(decoded.details[0].details[0].code == "leaf");
    CHECK(bridge::kVersion == 10);
}

TEST_CASE("malformed semantic schema falls back to plain CLAP parameters with a warning") {
    auto info = find("com.suppleaudio.test.malformed-schema");
    CHECK_FALSE(info.semantic);
    auto instance = Instance::create(info, {});
    REQUIRE(instance);
    CHECK_FALSE((*instance)->info().semantic);
    CHECK_FALSE((*instance)->semanticSchema());
    CHECK_FALSE((*instance)->params().empty());
    REQUIRE((*instance)->setParam(0, gainClap(-3)));
    CHECK(*(*instance)->paramValue(0) == doctest::Approx(gainClap(-3)));
    const auto log = (*instance)->takeLog();
    REQUIRE_FALSE(log.empty());
    CHECK(log[0].find("invalid semantic schema") != std::string::npos);
}

TEST_CASE("editor failures destroy the partial editor and show-hide requests reach its owner") {
    for (std::uint8_t mode : {3, 4}) {
        auto made = Instance::create(find("com.suppleaudio.test.edge"), {});
        REQUIRE(made);
        const std::array<std::uint8_t, 4> config{mode, 0, 0, 0};
        REQUIRE((*made)->loadState(config));
        CHECK_FALSE((*made)->openEditor(1234, 1));
        CHECK_FALSE((*made)->editorOpen());
        const std::array<std::uint8_t, 4> good{6, 0, 0, 0};
        REQUIRE((*made)->loadState(good));
        REQUIRE((*made)->openEditor(1234, 1));
        auto requests = (*made)->takeEditorRequests();
        CHECK(requests.show);
        CHECK(requests.hide);
        CHECK(requests.width == 320);
        CHECK(requests.height == 240);
        (*made)->closeEditor();
    }
}

TEST_CASE("restart activation failures are structured notifications") {
    auto made = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(made);
    REQUIRE((*made)->activate(48000, 1, 64));
    const std::array<std::uint8_t, 4> config{5, 0, 0, 0};
    REQUIRE((*made)->loadState(config));
    REQUIRE((*made)->serviceMainThread());
    CHECK_FALSE((*made)->active());
    const auto notes = (*made)->takeNotifications();
    REQUIRE(notes.restartError);
    CHECK(notes.restartError->code == "host.activate");
    CHECK_FALSE((*made)->takeNotifications().restartError);
}

TEST_CASE(
    "VST3 editors are sized before attached; run-loop callbacks skip handlers unregistered by an "
    "earlier callback") {
    void* module = ::dlopen(FIXTURE_VST3_MODULE, RTLD_NOW);
    REQUIRE(module);
    struct Close {
        void* module;
        ~Close() { ::dlclose(module); }
    } close{module};
    auto removed = reinterpret_cast<int (*)()>(::dlsym(module, "fixtureRemovedHandlerCalls"));
    auto timers = reinterpret_cast<int (*)()>(::dlsym(module, "fixtureTimerCalls"));
    auto fds = reinterpret_cast<int (*)()>(::dlsym(module, "fixtureFdCalls"));
    REQUIRE(removed);
    REQUIRE(timers);
    REQUIRE(fds);
    auto unsized = reinterpret_cast<int (*)()>(::dlsym(module, "fixtureAttachedUnsized"));
    REQUIRE(unsized);
    auto list = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(list);
    const auto info = std::find_if(list->begin(), list->end(), [](const auto& p) {
        return p.name == "Test VST3 Separate Gain";
    });
    REQUIRE(info != list->end());
    auto made = Instance::create(*info, {});
    REQUIRE(made);
    const auto removedBefore = removed(), timerBefore = timers(), fdBefore = fds();
    auto size = (*made)->openEditor(1234, 1);
    REQUIRE(size);
    // Its size is asked before it is attached, so a view that sizes itself in
    // attached() opens at its size, not its minimum.
    CHECK(unsized() == 0);
    CHECK(size->width == 300);
    CHECK(size->height == 200);
    (*made)->onFd(123, CLAP_POSIX_FD_READ);
    CHECK(fds() == fdBefore + 1);
    CHECK(removed() == removedBefore);
    while (timers() == timerBefore) (*made)->serviceMainThread();
    CHECK(timers() == timerBefore + 1);
    CHECK(removed() == removedBefore);
    (*made)->closeEditor();
}

TEST_CASE("VST3 notes preserve overlapping counts and ids and expand wildcard offs") {
    void* module = ::dlopen(FIXTURE_VST3_MODULE, RTLD_NOW);
    REQUIRE(module);
    struct Close {
        void* module;
        ~Close() { ::dlclose(module); }
    } close{module};
    auto offs = reinterpret_cast<int (*)(int)>(::dlsym(module, "fixtureNoteOffs"));
    REQUIRE(offs);
    auto list = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(list);
    auto info = std::find_if(list->begin(), list->end(),
                             [](const auto& p) { return p.name == "Test VST3 Sine"; });
    REQUIRE(info != list->end());
    auto made = Instance::create(*info, {});
    REQUIRE(made);
    REQUIRE((*made)->activate(48000, 1, 64));
    REQUIRE((*made)->startProcessing());
    Buffers buffers(64);
    const int before = offs(-1), wildcards = offs(-2), id1 = offs(1), id2 = offs(2);
    std::array<Event, 4> notes{};
    for (auto& note : notes) note = {.kind = Event::Kind::NoteOn, .channel = 0, .key = 69};
    notes[2].noteId = 1;
    notes[3].noteId = 2;
    REQUIRE((*made)->process(buffers.block(64, notes, false)));
    Event stop{.kind = Event::Kind::NoteOff, .channel = 0, .key = 69, .noteId = 1};
    REQUIRE((*made)->process(buffers.block(64, std::span(&stop, 1), false)));
    CHECK(offs(1) == id1 + 1);
    (*made)->reset();
    REQUIRE((*made)->process(buffers.block(64, {}, false)));
    CHECK(offs(-1) == before + 4);
    CHECK(offs(2) == id2 + 1);
    CHECK(offs(-2) == wildcards);
    REQUIRE((*made)->process(buffers.block(64, notes, false)));
    stop = {.kind = Event::Kind::NoteChoke, .port = -1, .channel = -1, .key = -1};
    REQUIRE((*made)->process(buffers.block(64, std::span(&stop, 1), false)));
    CHECK(offs(-1) == before + 8);
    CHECK(offs(-2) == wildcards);
    (*made)->stopProcessing();
    (*made)->deactivate();
}

TEST_CASE("CLAP layouts beyond the supported port and channel limits are refused") {
    for (const auto* id :
         {"com.suppleaudio.test.many-ports", "com.suppleaudio.test.many-channels"}) {
        auto made = Instance::create(find(id), {});
        REQUIRE_FALSE(made);
        CHECK(made.error().code == "host.layout");
    }
    RemotePorts ports;
    auto remote = RemoteInstance::spawn(find("com.suppleaudio.test.gain"), {}, ports);
    REQUIRE(remote);
    auto activated = (*remote)->activate(48000, 1, 64, 17, 2);
    REQUIRE_FALSE(activated);
    CHECK(activated.error().code == "host.layout");
}

TEST_CASE("VST3 layouts beyond the supported bus and channel limits are refused") {
    void* module = ::dlopen(FIXTURE_VST3_MODULE, RTLD_NOW);
    REQUIRE(module);
    auto mode = reinterpret_cast<void (*)(int)>(::dlsym(module, "fixtureLayoutMode"));
    REQUIRE(mode);
    struct Cleanup {
        void* module;
        void (*mode)(int);
        ~Cleanup() {
            mode(0);
            ::dlclose(module);
        }
    } cleanup{module, mode};
    auto list = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(list);
    for (int layout : {1, 2}) {
        mode(layout);
        auto made = Instance::create(list->front(), {});
        REQUIRE_FALSE(made);
        CHECK(made.error().code == "host.layout");
    }
}

TEST_CASE("editor resize requests publish dimensions as one pair") {
    void* module = ::dlopen(kFixture.c_str(), RTLD_NOW);
    REQUIRE(module);
    struct Close {
        void* module;
        ~Close() { ::dlclose(module); }
    } close{module};
    auto stress = reinterpret_cast<void (*)()>(::dlsym(module, "supple_fixture_resize_stress"));
    REQUIRE(stress);
    auto made = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(made);
    REQUIRE((*made)->openEditor(1234, 1));
    (void)(*made)->takeEditorRequests();
    std::atomic<bool> done{false};
    std::thread producer([&] {
        stress();
        done.store(true, std::memory_order_release);
    });
    bool consistent = true;
    auto check = [&] {
        const auto request = (*made)->takeEditorRequests();
        if (request.resize && request.height != (request.width ^ UINT32_MAX)) consistent = false;
    };
    while (!done.load(std::memory_order_acquire)) check();
    producer.join();
    check();
    CHECK(consistent);
    (*made)->closeEditor();
}

TEST_CASE("valid bridge errors preserve details without failing the process") {
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    auto process = BridgeTestAccess::adopt(sockets[0], -1);
    std::thread peer([&] {
        std::uint32_t head = 0, id = 0;
        std::vector<std::uint8_t> payload;
        if (bridge::recvFrame(sockets[1], head, id, payload)) {
            Error error("test.invalid", "invalid value");
            error.details.emplace_back("test.range", "out of range", "/gain", "use 0..1");
            bridge::Writer writer;
            writer.error(error);
            bridge::sendFrame(sockets[1], 1, id, writer.bytes);
        }
        ::close(sockets[1]);
    });
    auto result = process->call(bridge::Op::SetParam, 1, bridge::Writer{});
    peer.join();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "test.invalid");
    REQUIRE(result.error().details.size() == 1);
    CHECK(result.error().details[0].path == "/gain");
    CHECK(result.error().details[0].hint == "use 0..1");
    CHECK_FALSE(process->failed());
}

TEST_CASE("CLAP process requests and ordered parameter clear notifications are observable") {
    struct Control {
        void (*requestProcess)(const clap_plugin_t*);
        void (*rescan)(const clap_plugin_t*, bool);
        void (*clear)(const clap_plugin_t*, clap_id, clap_param_clear_flags);
        void (*restart)(const clap_plugin_t*);
    };
    auto made = Instance::create(find("com.suppleaudio.test.edge"), {});
    REQUIRE(made);
    auto& instance = **made;
    const auto* plugin = instance.raw();
    const auto* control =
        static_cast<const Control*>(plugin->get_extension(plugin, "supple.test.edge-control"));
    REQUIRE(control);
    CHECK_FALSE(instance.processRequested());
    control->requestProcess(plugin);
    CHECK(instance.processRequested());
    CHECK(instance.takeProcessRequest());
    CHECK_FALSE(instance.takeProcessRequest());
    control->clear(plugin, 7, CLAP_PARAM_CLEAR_AUTOMATIONS);
    control->clear(plugin, 8, CLAP_PARAM_CLEAR_MODULATIONS);
    auto notes = instance.takeNotifications();
    REQUIRE(notes.paramsClear.size() == 2);
    CHECK(notes.paramsClear[0].id == 7);
    CHECK(notes.paramsClear[0].flags == CLAP_PARAM_CLEAR_AUTOMATIONS);
    CHECK(notes.paramsClear[1].id == 8);
    CHECK(notes.paramsClear[1].flags == CLAP_PARAM_CLEAR_MODULATIONS);
    CHECK(instance.takeNotifications().paramsClear.empty());
    // Rescan wins over earlier clears; a subsequent clear remains effective.
    control->clear(plugin, 7, CLAP_PARAM_CLEAR_ALL);
    control->rescan(plugin, false);
    control->clear(plugin, 8, CLAP_PARAM_CLEAR_ALL);
    notes = instance.takeNotifications();
    CHECK(notes.paramsRescan);
    CHECK(notes.paramsRescanFlags == CLAP_PARAM_RESCAN_ALL);
    REQUIRE(notes.paramsClear.size() == 1);
    CHECK(notes.paramsClear[0].id == 8);
}

TEST_CASE("bridged process requests and ordered clears survive service and rescan") {
    auto made = Instance::create(find("com.suppleaudio.test.edge"), {}, true);
    REQUIRE(made);
    auto& instance = **made;
    CHECK_FALSE(instance.processRequested());
    REQUIRE(instance.loadState(std::array<std::uint8_t, 4>{20, 0, 0, 0}));
    instance.serviceMainThread();
    CHECK(instance.processRequested());
    CHECK(instance.takeProcessRequest());
    instance.serviceMainThread();
    CHECK_FALSE(instance.takeProcessRequest());
    auto notes = instance.takeNotifications();
    REQUIRE(notes.paramsClear.size() == 2);
    CHECK(notes.paramsClear[0].id == 7);
    CHECK(notes.paramsClear[0].flags == CLAP_PARAM_CLEAR_AUTOMATIONS);
    CHECK(notes.paramsClear[1].id == 8);
    CHECK(notes.paramsClear[1].flags == CLAP_PARAM_CLEAR_MODULATIONS);
    REQUIRE(instance.loadState(std::array<std::uint8_t, 4>{20, 0, 0, 0}));
    instance.serviceMainThread();  // leave earlier clears pending in the proxy
    REQUIRE(instance.loadState(std::array<std::uint8_t, 4>{21, 0, 0, 0}));
    instance.serviceMainThread();
    notes = instance.takeNotifications();
    CHECK(notes.paramsRescan);
    CHECK(notes.paramsRescanFlags == CLAP_PARAM_RESCAN_ALL);
    REQUIRE(notes.paramsClear.size() == 1);
    CHECK(notes.paramsClear[0].id == 8);
    CHECK(notes.paramsClear[0].flags == CLAP_PARAM_CLEAR_ALL);
    CHECK(instance.takeNotifications().paramsClear.empty());
}

TEST_CASE("bridge service rejects oversized and truncated clear lists") {
    for (const auto count : {bridge::kMaxParamClears + 1, 1U}) {
        bridge::Writer w;
        for (int i = 0; i < 6; ++i) w.u8(0);
        w.u32(0);
        w.u8(1);
        w.u32(count);
        CHECK_FALSE(bridge::validReply(bridge::Op::Service,
                                       bridge::Reader(w.bytes.data(), w.bytes.size())));
    }
}

TEST_CASE("semantic IDs absent from CLAP degrade to plain parameters with a warning") {
    for (bool isolated : {false, true}) {
        auto info = find("com.suppleaudio.test.semantic-id-mismatch");
        CHECK_FALSE(info.semantic);
        auto made = Instance::create(info, {}, isolated);
        REQUIRE(made);
        CHECK_FALSE((*made)->info().semantic);
        CHECK_FALSE((*made)->semanticSchema());
        REQUIRE((*made)->paramValue(0));
        (*made)->serviceMainThread();
        auto logs = (*made)->takeLog();
        CHECK(std::any_of(logs.begin(), logs.end(), [](const auto& line) {
            return line.find("invalid semantic schema") != std::string::npos;
        }));
    }
}

TEST_CASE("oversized acknowledged bridge output count fails silently without arena traversal") {
    for (int sinkSize : {-1, 1, static_cast<int>(bridge::kMaxOutEvents + 1)}) {
        bridge::Layout layout(64, 2, 2, 0, 2);
        void* arena =
            ::mmap(nullptr, layout.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        REQUIRE(arena != MAP_FAILED);
        auto* header = new (arena) bridge::Header{};
        auto process = BridgeTestAccess::adopt(-1, -1);
        auto instance = BridgeTestAccess::dataPlane(process, arena, layout);
        auto* output =
            reinterpret_cast<float*>(static_cast<std::uint8_t*>(arena) + layout.outAudio);
        auto* aux =
            reinterpret_cast<float*>(static_cast<std::uint8_t*>(arena) + layout.auxOutAudio);
        std::fill_n(output, 128, .5f);
        std::fill_n(aux, 128, .5f);
        // A scripted peer acknowledges corrupt output exactly as a bridge would.
        std::thread peer([&] {
            while (header->request.load(std::memory_order_acquire) == 0) std::this_thread::yield();
            header->outEventCount = bridge::kMaxOutEvents + 1;
            header->result = 1;
            header->done.store(header->request.load(), std::memory_order_release);
            bridge::futexWake(header->done);
        });
        Buffers buffers(64);
        auto block = buffers.block(64);
        std::array<float, 64> auxL{}, auxR{};
        float* auxOut[]{auxL.data(), auxR.data()};
        block.auxOutputs = auxOut;
        block.auxOutputChannels = 2;
        std::vector<OutputEvent> storage(static_cast<std::size_t>(std::max(0, sinkSize)));
        OutputEvents events{storage};
        const auto ok = instance->process(block, sinkSize < 0 ? nullptr : &events);
        peer.join();
        CHECK(ok);  // failed plug-in is silenced while its session keeps playing
        CHECK(instance->failed());
        CHECK(instance->failure().find("protocol") != std::string::npos);
        CHECK(events.count == 0);
        CHECK(events.dropped == 0);
        CHECK(std::all_of(buffers.ol.begin(), buffers.ol.end(), [](float x) { return x == 0; }));
        CHECK(std::all_of(auxL.begin(), auxL.end(), [](float x) { return x == 0; }));
        CHECK(instance->process(block, nullptr));  // permanently silent until re-created
        const auto query = instance->paramValue(0);
        REQUIRE_FALSE(query);
        CHECK(query.error().code == "host.bridge_failed");
        CHECK(query.error().message.find("protocol") != std::string::npos);
        CHECK_FALSE(query.error().hint.empty());
    }
}

TEST_CASE("a VST3 state its controller rejects leaves the plug-in as it was") {
    auto list = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(list);
    const auto info = std::find_if(list->begin(), list->end(), [](const auto& p) {
        return p.name == "Test VST3 Separate Gain";
    });
    REQUIRE(info != list->end());
    auto made = Instance::create(*info, {});
    REQUIRE(made);
    auto state = (*made)->saveState();
    REQUIRE(state);
    const auto before = (*made)->paramValue(1);
    REQUIRE(before);
    // The component's 8-byte value changed to 0.9, then a one-byte
    // controller state the controller refuses (framing stays valid).
    const double changed = 0.9;
    std::memcpy(state->data() + 16, &changed, 8);
    state->resize(25);
    (*state)[12] = 1;
    (*state)[13] = (*state)[14] = (*state)[15] = 0;
    (*state)[24] = 0;
    auto loaded = (*made)->loadState(*state);
    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code == "host.state");
    CHECK_FALSE(loaded.error().hint.empty());
    const auto after = (*made)->paramValue(1);
    REQUIRE(after);
    CHECK(*after == doctest::Approx(*before));
}

TEST_CASE("a VST3 that settles on another layout is served it, in process or isolated") {
    setBridgeExecutable(BRIDGE_PATH);
    auto found = scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    auto adapter = std::find_if(found->begin(), found->end(), [](const PluginInfo& p) {
        return p.name == "Test VST3 Mono Adapter";
    });
    REQUIRE(adapter != found->end());
    for (bool isolated : {false, true}) {
        CAPTURE(isolated);
        auto made = Instance::create(*adapter, {}, isolated);
        REQUIRE(made);
        Instance& p = **made;
        CHECK(p.audioOutputChannels() == 2);  // offered stereo
        auto activated = p.activate(48000, 1, 64);
        REQUIRE_MESSAGE(activated, (activated ? std::string() : activated.error().toString()));
        CHECK(p.audioInputChannels() == 1);  // settled on mono
        CHECK(p.audioOutputChannels() == 1);
        REQUIRE(p.startProcessing());
        std::vector<float> in(32, 1.0f), out(32, 0.0f);
        const float* ins[1] = {in.data()};
        float* outs[1] = {out.data()};
        ProcessBlock b;
        b.inputs = ins;
        b.inputChannels = 1;
        b.outputs = outs;
        b.outputChannels = 1;
        b.frames = 32;
        REQUIRE(p.process(b));
        CHECK(out[31] == doctest::Approx(0.5));
        p.stopProcessing();
        p.deactivate();
    }
}
