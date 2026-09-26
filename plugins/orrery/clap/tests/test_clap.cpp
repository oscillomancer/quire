// Orrery through the real CLAP host: discovery, the semantic extension,
// rendering notes, parameter automation and state.
#include <doctest/doctest.h>

#include <clap/clap.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "supple/clap/semantic_params.h"
#include "supple/host/clap_host.h"
#include "supple/json/json.h"

using namespace supple;

namespace {

constexpr std::string_view kPad = R"({"schema": "supple.orrery-patch/1.0", "seed": 5,
  "voice": {"unison": 2},
  "oscillators": {"osc1": {"type": "va", "wave": "saw"}, "osc2": {"type": "sub"}},
  "filters": {"filter1": {"type": "ladder", "cutoff": "900 Hz", "resonance": "30 %"}},
  "envelopes": {"amp": {"attack": "5 ms", "release": "80 ms"}, "env2": {"decay": "200 ms", "sustain": 0}},
  "routes": {"env2>filter1.cutoff": {"amount": "+2 oct"}},
  "fx": {"chain": ["reverb"], "reverb": {"mix": "15 %"}}})";

std::unique_ptr<host::Instance> load() {
    const std::filesystem::path path = ORRERY_CLAP_PATH;
    auto infos = host::scan(std::span<const std::filesystem::path>(&path, 1));
    REQUIRE(infos);
    REQUIRE(infos->size() == 1);
    CHECK((*infos)[0].id == "com.suppleaudio.orrery");
    auto inst = host::Instance::create((*infos)[0], host::HostIdentity{});
    REQUIRE_MESSAGE(inst, (inst ? "" : inst.error().message));
    return std::move(*inst);
}

double renderRms(host::Instance& in, std::vector<host::Event> events, int blocks) {
    std::vector<float> l(256), r(256);
    float* outs[2] = {l.data(), r.data()};
    double sum = 0.0;
    std::size_t n = 0;
    for (int b = 0; b < blocks; ++b) {
        host::ProcessBlock block;
        block.outputs = outs;
        block.outputChannels = 2;
        block.frames = 256;
        if (b == 0) block.events = events;
        REQUIRE(in.process(block));
        for (float x : l) sum += static_cast<double>(x) * static_cast<double>(x);
        n += l.size();
    }
    return std::sqrt(sum / static_cast<double>(n));
}

std::uint32_t idOf(host::Instance& in, std::string_view name) {
    for (const auto& p : in.params())
        if (p.name == name) return p.id;
    FAIL("no parameter " << name);
    return 0;
}

}  // namespace

TEST_CASE("Orrery loads as a CLAP instrument with the semantic extension") {
    auto in = load();
    CHECK(in->acceptsNotes());
    CHECK(in->audioOutputChannels() == 2);
    REQUIRE(in->activate(48000.0, 1, 256));
    auto schema = in->semanticSchema();
    REQUIRE(schema);
    CHECK(schema->find("patch_type")->asString() == "supple.orrery-patch/1.0");
    CHECK(schema->find("params")->find("filter1.cutoff") != nullptr);
    // Defaults: one va oscillator — osc2's parameters are inactive.
    CHECK(schema->find("params")->find("osc2.level")->find("active")->asBool() == false);
    CHECK(schema->find("params")->find("osc1.level")->find("active") == nullptr);
    CHECK(in->params().size() > 200);

    REQUIRE(in->setPatch(kPad, CLAP_SUPPLE_SET_REPLACE));
    auto minimal = in->getPatch(true);
    REQUIRE(minimal);
    CHECK(minimal->find("\"ladder\"") != std::string::npos);
    auto d = in->describe(true);
    REQUIRE(d);
    CHECK(d->find("ladder 24 dB LP") != std::string::npos);

    // Invalid patches are rejected with a path; nothing changes.
    auto bad = in->setPatch(R"({"filters": {"filter1": {"cutof": 300}}})", CLAP_SUPPLE_SET_MERGE);
    CHECK_FALSE(bad);
    CHECK(bad.error().message.find("cutof") != std::string::npos);
    CHECK(*in->getPatch(true) == *minimal);
}

TEST_CASE("notes render; automation moves the parameter; state round-trips") {
    auto in = load();
    REQUIRE(in->activate(48000.0, 1, 256));
    REQUIRE(in->setPatch(kPad, CLAP_SUPPLE_SET_REPLACE));
    REQUIRE(in->startProcessing());
    host::Event on;
    on.kind = host::Event::Kind::NoteOn;
    on.key = 57;
    on.velocity = 0.9;
    const double loud = renderRms(*in, {on}, 40);
    CHECK(loud > 0.01);
    host::Event off = on;
    off.kind = host::Event::Kind::NoteOff;
    renderRms(*in, {off}, 1000);  // release + reverb tail (2 s decay)
    CHECK(renderRms(*in, {}, 20) < 1e-4);

    // Host automation of the cutoff reaches the patch.
    const std::uint32_t cutoff = idOf(*in, "Filter 1 Cutoff");
    host::Event p;
    p.kind = host::Event::Kind::Param;
    p.paramId = cutoff;
    p.value = 0.25;  // normalized (log curve)
    renderRms(*in, {p}, 2);
    in->stopProcessing();
    auto patch = json::parse(*in->getPatch(false));
    REQUIRE(patch);
    const double hz = patch->find("filters")->find("filter1")->find("cutoff")->asDouble();
    CHECK(hz == doctest::Approx(20.0 * std::pow(1000.0, 0.25)).epsilon(1e-6));

    auto state = in->saveState();
    REQUIRE(state);
    auto other = load();
    REQUIRE(other->loadState(*state));
    CHECK(*other->getPatch(false) == *in->getPatch(false));
}
