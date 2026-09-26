#include <doctest/doctest.h>

#include <cmath>

#include <supple/engine/offline.h>

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;
constexpr const char* kSine = "com.suppleaudio.test.sine";
constexpr const char* kGain = "com.suppleaudio.test.gain";

InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

Id addTrack(model::EditTxn& t, std::string name, Id parent = kMaster) {
    model::Track tr;
    tr.name = std::move(name);
    tr.parent = parent;
    const Id id = t.create(tr);
    t.modify<model::Track>(parent, [&](model::Track& p) { p.children.push_back(id); });
    return id;
}

Id addFx(model::EditTxn& t, Id track, const char* plugin, std::optional<json::Value> patch = {}) {
    model::FxInstance fx;
    fx.plugin.id = plugin;
    fx.patch = std::move(patch);
    const Id id = t.create(fx);
    t.modify<model::Track>(track, [&](model::Track& tr) { tr.chain.push_back(id); });
    return id;
}

// A MIDI item on `track` with one note (beats).
void addNote(model::EditTxn& t, Id track, double startBeat, double lengthBeats, int key = 69,
             int velocity = 127) {
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = static_cast<std::int64_t>(lengthBeats * 960);
    src.sequence.notes = {{0, static_cast<std::int64_t>(lengthBeats * 960), 0,
                           static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(velocity), 0}};
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.position = startBeat;
    item.length = lengthBeats;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
}

model::Session commit(model::EditTxn& t) {
    auto s = t.commit();
    REQUIRE_MESSAGE(s, (s ? std::string() : s.error().toString()));
    return std::move(*s);
}

double peak(const std::vector<float>& v, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    double m = 0;
    for (std::size_t i = from; i < std::min(to, v.size()); ++i)
        m = std::max(m, std::fabs(double(v[i])));
    return m;
}

std::size_t firstNonZero(const std::vector<float>& v) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] != 0.0f) return i;
    return v.size();
}

OfflineResult render(const model::Session& s, InstancePool& pool, double seconds, double tail = 0) {
    auto r = renderOffline(s, pool, {0.0, seconds, tail, 512});
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    return std::move(*r);
}

constexpr double kCentre = 0.70710678118654752;  // -3 dB pan law at the centre

}  // namespace

TEST_CASE("pan law gains") {
    auto [l, r] = panGains(0.0, -3.0);
    CHECK(l == doctest::Approx(kCentre).epsilon(1e-6));
    CHECK(r == doctest::Approx(kCentre).epsilon(1e-6));
    std::tie(l, r) = panGains(-1.0, -3.0);
    CHECK(l == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(std::fabs(r) < 1e-12);
    std::tie(l, r) = panGains(0.0, 0.0);
    CHECK(l == doctest::Approx(1.0).epsilon(1e-6));
}

TEST_CASE("an instrument track plays its MIDI item at the right samples" *
          doctest::description("rt")) {
    auto pool = makePool();
    model::EditTxn t(model::Session::create(), 1);
    const Id tr = addTrack(t, "Synth");
    addFx(t, tr, kSine);
    addNote(t, tr, 1.0, 1.0);  // beat 1..2 at 120 BPM = 0.5 s .. 1.0 s
    auto s = commit(t);
    auto r = render(s, pool, 2.0);
    REQUIRE(r.audio.frames() == 96000);
    CHECK(r.outputLatency == 0);
    const auto& L = r.audio.channels[0];
    CHECK(firstNonZero(L) == 24001);  // phase 0 at the note-on sample
    CHECK(peak(L, 24000, 48000) == doctest::Approx(0.25 * kCentre).epsilon(1e-4));
    CHECK(peak(L, 48000) == 0.0);  // the fixture stops at note off
    // Deterministic.
    auto again = render(s, pool, 2.0);
    CHECK(again.audio.channels == r.audio.channels);
}

TEST_CASE("mixer: volume, folder, hard pan, mute, pre-fader route") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create(), 1);
    const Id folder = addTrack(t, "Folder");
    const Id a = addTrack(t, "A", folder);
    addFx(t, a, kSine);
    addNote(t, a, 0.0, 1.0);
    t.modify<model::Track>(a, [](model::Track& x) { x.volumeDb = -6.0; });
    t.modify<model::Track>(folder, [](model::Track& x) { x.pan = -100.0; });
    auto s = commit(t);
    auto r = render(s, pool, 0.5);
    // A at -6 dB, centred (0.707), then the folder hard left (1.0 / 0.0).
    const double expect = 0.25 * std::pow(10.0, -6.0 / 20.0) * kCentre;
    CHECK(peak(r.audio.channels[0]) == doctest::Approx(expect).epsilon(1e-4));
    CHECK(peak(r.audio.channels[1]) < 1e-9);

    // Mute A but send it pre-fader to a bus: the bus still hears it.
    model::EditTxn t2(s, 2);
    const Id bus = addTrack(t2, "Bus");
    t2.modify<model::Track>(a, [](model::Track& x) { x.mute = true; });
    model::Route send;
    send.from.track = a;
    send.to.track = bus;
    send.tap = model::Tap::PreFader;
    t2.create(send);
    auto s2 = commit(t2);
    auto r2 = render(s2, pool, 0.5);
    // Pre-fader: no -6 dB; the route pans centre (0.707), the bus centre (0.707).
    CHECK(peak(r2.audio.channels[0]) == doctest::Approx(0.25 * kCentre * kCentre).epsilon(1e-4));
}

TEST_CASE("latency compensation keeps parallel paths aligned") {
    auto pool = makePool();
    auto build = [](bool withLatent) {
        model::EditTxn t(model::Session::create(), 1);
        const Id b = addTrack(t, "Plain");
        addFx(t, b, kSine);
        addNote(t, b, 0.0, 1.0);
        if (withLatent) {
            const Id a = addTrack(t, "Latent");
            addFx(t, a, kSine);
            addFx(t, a, kGain,
                  json::parse(
                      R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
                      .value());
            addNote(t, a, 0.0, 1.0);
        }
        auto s = t.commit();
        REQUIRE(s);
        return std::move(*s);
    };
    auto both = render(build(true), pool, 0.5);
    auto single = render(build(false), pool, 0.5);
    CHECK(both.outputLatency == 64);
    CHECK(single.outputLatency == 0);
    REQUIRE(both.audio.frames() == single.audio.frames());
    // Aligned to the sample: exactly twice the single track (no comb filtering).
    double worst = 0;
    for (std::size_t i = 0; i < single.audio.frames(); ++i)
        worst = std::max(worst, std::fabs(double(both.audio.channels[0][i]) -
                                          2.0 * double(single.audio.channels[0][i])));
    CHECK(worst < 1e-6);
    CHECK(firstNonZero(both.audio.channels[0]) == firstNonZero(single.audio.channels[0]));
}

TEST_CASE("the tempo map places notes") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create(), 1);
    t.create(model::TempoPointObj{4.0, 60.0, false});  // 120 BPM for 4 beats (2 s), then 60
    const Id tr = addTrack(t, "Synth");
    addFx(t, tr, kSine);
    addNote(t, tr, 5.0, 0.5);  // 2 s + 1 beat at 60 = 3 s
    auto s = commit(t);
    auto r = render(s, pool, 4.0);
    CHECK(firstNonZero(r.audio.channels[0]) == 3 * 48000 + 1);
}

TEST_CASE("a missing plugin passes audio through with a warning") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create(), 1);
    const Id tr = addTrack(t, "Synth");
    addFx(t, tr, kSine);
    addFx(t, tr, "org.example.not-installed");
    addNote(t, tr, 0.0, 1.0);
    auto s = commit(t);
    auto r = render(s, pool, 0.5);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("org.example.not-installed") != std::string::npos);
    CHECK(peak(r.audio.channels[0]) == doctest::Approx(0.25 * kCentre).epsilon(1e-4));
    CHECK(renderOffline(s, pool, {1.0, 1.0, 0, 512}).error().code == "engine.range");
}
