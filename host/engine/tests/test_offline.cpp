#include <doctest/doctest.h>
#include <unistd.h>

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
    auto r =
        renderOffline(s, pool, {.startSeconds = 0.0, .endSeconds = seconds, .tailSeconds = tail});
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
    CHECK(renderOffline(s, pool, {.startSeconds = 1.0, .endSeconds = 1.0}).error().code ==
          "engine.range");
}

namespace {
Id addEnvelope(model::EditTxn& t, Id object, std::string param,
               std::vector<model::EnvPoint> points) {
    model::Envelope env;
    env.target = {object, std::move(param)};
    env.points = std::move(points);
    return t.create(env);
}
double windowPeak(const std::vector<float>& v, std::size_t centre) {
    return peak(v, centre - 120, centre + 120);
}
}  // namespace

TEST_CASE("mixer automation: volume in dB, square mute on the exact sample, route mute") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create(), 1);
    const Id tr = addTrack(t, "Synth");
    addFx(t, tr, kSine);
    addNote(t, tr, 0.0, 8.0);
    addEnvelope(t, tr, "volume", {{0.0, 0.0}, {2.0, -12.0}});
    addEnvelope(t, tr, "mute", {{0.0, 0.0, model::PointShape::Square}, {3.0, 1.0}});
    auto s = commit(t);
    auto r = render(s, pool, 2.0);
    REQUIRE(r.warnings.empty());
    const auto& L = r.audio.channels[0];
    const double base = 0.25 * kCentre;
    CHECK(windowPeak(L, 24000) / base ==
          doctest::Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(0.01));
    CHECK(windowPeak(L, 60000) / base ==
          doctest::Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(0.01));
    CHECK(peak(L, 72000) == 0.0);  // muted from beat 3 = 1.5 s
    CHECK(peak(L, 71800, 72000) > 0.0);

    // Through a latent chain the mute still lands on beat 3 of the output.
    model::EditTxn t2(s, 2);
    addFx(t2, tr, kGain,
          json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
              .value());
    auto r2 = render(commit(t2), pool, 2.0);
    CHECK(r2.outputLatency == 64);
    CHECK(peak(r2.audio.channels[0], 72000) == 0.0);
    CHECK(peak(r2.audio.channels[0], 71800, 72000) > 0.0);
}

TEST_CASE("route automation") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create(), 1);
    const Id src = addTrack(t, "Src");
    addFx(t, src, kSine);
    addNote(t, src, 0.0, 4.0);
    t.modify<model::Track>(src, [](model::Track& x) { x.mute = true; });
    const Id bus = addTrack(t, "Bus");
    model::Route send;
    send.from.track = src;
    send.to.track = bus;
    send.tap = model::Tap::PreFader;
    const Id sd = t.create(send);
    addEnvelope(t, sd, "mute", {{0.0, 0.0, model::PointShape::Square}, {1.0, 1.0}});
    auto r = render(commit(t), pool, 1.0);
    CHECK(peak(r.audio.channels[0], 0, 24000) > 0.1);
    CHECK(peak(r.audio.channels[0], 24000) == 0.0);
}

TEST_CASE("solo: soloed tracks and their signal path stay audible") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create(), 1);
    const Id folder = addTrack(t, "Folder");
    const Id a = addTrack(t, "A", folder);
    const Id b = addTrack(t, "B");
    addFx(t, a, kSine);
    addFx(t, b, kSine);
    addNote(t, a, 0.0, 1.0, 69);
    addNote(t, b, 0.0, 1.0, 81);
    t.modify<model::Track>(a, [](model::Track& x) { x.solo = model::Solo::On; });
    auto s = commit(t);
    auto r = render(s, pool, 0.5);
    // Only A (through its folder) is heard: one sine at the -3 dB law, twice.
    CHECK(peak(r.audio.channels[0]) == doctest::Approx(0.25 * kCentre * kCentre).epsilon(1e-4));
    model::EditTxn t2(s, 2);
    t2.modify<model::Track>(b, [](model::Track& x) { x.solo = model::Solo::Defeat; });
    auto r2 = render(commit(t2), pool, 0.5);
    CHECK(peak(r2.audio.channels[0]) > 0.25 * kCentre * kCentre * 1.2);  // B is back
}

TEST_CASE("audio items: position, take offset, fade, channel mode, offline media") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-media-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData ramp;
    ramp.sampleRate = 48000;
    ramp.channels.assign(2, std::vector<float>(48000));
    for (int i = 0; i < 48000; ++i) {
        ramp.channels[0][static_cast<std::size_t>(i)] = static_cast<float>(i) / 48000.0f;
        ramp.channels[1][static_cast<std::size_t>(i)] = -static_cast<float>(i) / 48000.0f;
    }
    REQUIRE(audio::writeWav(dir / "media" / "ramp.wav", ramp).ok());
    ramp.sampleRate = 44100;
    REQUIRE(audio::writeWav(dir / "media" / "slow.wav", ramp).ok());

    model::EditTxn t(model::Session::create(), 1);
    const Id tr = addTrack(t, "Audio");
    const Id md = t.create(model::MediaSource{"ramp.wav", "", "wav", 48000, 2, 48000});
    model::Take take;
    take.source = md;
    take.startOffset = 0.1;  // 4800 frames in
    const Id tk = t.create(take);
    model::Item item;
    item.track = tr;
    item.position = 1.0;  // beat 1 = 0.5 s
    item.length = 0.5;    // beats = 0.25 s
    item.takes = {tk};
    item.activeTake = tk;
    item.fadeIn = {0.01, model::FadeShape::Linear, 0};  // 480 samples
    const Id it = t.create(item);
    auto s = commit(t);

    auto pool = makePool();
    MediaCache media;
    auto r = renderOffline(
        s, pool, {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir, .media = &media});
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    CHECK(r->warnings.empty());
    const auto& L = r->audio.channels[0];
    const auto& R = r->audio.channels[1];
    CHECK(peak(L, 0, 24000) == 0.0);
    CHECK(peak(L, 36000) == 0.0);  // the item ends at 0.75 s
    // After the fade: source frame 4800 + k, centred (0.707).
    for (int k : {480, 1000, 11999}) {
        const auto i = static_cast<std::size_t>(24000 + k);
        const double src = (4800.0 + k) / 48000.0;
        CHECK(double(L[i]) == doctest::Approx(src * kCentre).epsilon(1e-5));
        CHECK(double(R[i]) == doctest::Approx(-src * kCentre).epsilon(1e-5));
    }
    CHECK(double(L[24000 + 240]) ==
          doctest::Approx(0.5 * (4800.0 + 240) / 48000.0 * kCentre).epsilon(1e-5));
    CHECK(media.size() == 1);

    model::EditTxn t2(s, 2);
    t2.modify<model::Take>(tk, [](model::Take& x) { x.channelMode = model::ChannelMode::Left; });
    auto r2 =
        renderOffline(commit(t2), pool,
                      {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir, .media = &media});
    REQUIRE(r2);
    CHECK(r2->audio.channels[1][30000] == r2->audio.channels[0][30000]);

    model::EditTxn t3(s, 2);
    t3.modify<model::MediaSource>(md, [](model::MediaSource& m) { m.path = "slow.wav"; });
    (void)it;
    auto r3 =
        renderOffline(commit(t3), pool, {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir});
    REQUIRE(r3);
    REQUIRE(r3->warnings.size() == 1);
    CHECK(r3->warnings[0].find("44100") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("offline renders are bit-identical for any thread count (B4)") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create(), 1);
    const Id folder = addTrack(t, "Folder");
    const Id busA = addTrack(t, "Bus A");
    const Id busB = addTrack(t, "Bus B");
    addFx(t, busB, kGain,
          json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
              .value());
    for (int i = 0; i < 10; ++i) {
        const Id tr = addTrack(t, "T" + std::to_string(i), i % 3 == 0 ? folder : kMaster);
        addFx(t, tr, kSine);
        if (i % 4 == 1)
            addFx(t, tr, kGain,
                  json::parse(
                      R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
                      .value());
        addNote(t, tr, 0.25 * i, 2.0, 48 + 3 * i, 60 + 5 * i);
        t.modify<model::Track>(tr, [&](model::Track& x) {
            x.volumeDb = -1.0 * i;
            x.pan = -90.0 + 20.0 * i;
        });
        model::Route send;
        send.from.track = tr;
        send.to.track = i % 2 ? busA : busB;
        send.tap = i % 3 ? model::Tap::PostFader : model::Tap::PreFader;
        send.volumeDb = -3.0 - i;
        t.create(send);
    }
    const auto s = commit(t);
    std::vector<OfflineResult> runs;
    for (unsigned threads : {1u, 2u, 8u}) {
        auto r =
            renderOffline(s, pool, {.startSeconds = 0.0, .endSeconds = 2.0, .threads = threads});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        runs.push_back(std::move(*r));
    }
    CHECK(peak(runs[0].audio.channels[0]) > 0.01);
    for (const OfflineResult& r : runs) {
        CHECK(r.audio.channels[0] == runs[0].audio.channels[0]);
        CHECK(r.audio.channels[1] == runs[0].audio.channels[1]);
    }
}
