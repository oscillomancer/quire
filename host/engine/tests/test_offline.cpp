#include <doctest/doctest.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <thread>

#include <supple/engine/offline.h>

#include "plan.h"

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

// The frequency of a tone, from its zero crossings in [from, to).
double frequency(const std::vector<float>& v, std::size_t from, std::size_t to) {
    int crossings = 0;
    for (std::size_t i = from + 1; i < std::min(to, v.size()); ++i)
        if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) ++crossings;
    return crossings * 48000.0 / (2.0 * static_cast<double>(to - from));
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

TEST_CASE("shared bridge accepts a new effect after its last instance is removed") {
    host::setBridgeExecutable(BRIDGE_PATH);
    model::EditTxn initial(model::Session::create(), 1);
    const Id track = addTrack(initial, "Synth");
    addFx(initial, track, kSine);
    addNote(initial, track, 0.0, 1.0);
    const model::Session base = commit(initial);
    auto pool = makePool();
    const auto dry = render(base, pool, 0.05);
    REQUIRE(peak(dry.audio.channels[0]) > 0.1);

    int sharedPid = -1;
    for (int cycle = 0; cycle < 24; ++cycle) {
        model::EditTxn edit(base, static_cast<std::uint64_t>(cycle + 2));
        const Id fx = addFx(
            edit, track, kGain,
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":"-60 dB"}})")
                .value());
        edit.modify<model::FxInstance>(
            fx, [](model::FxInstance& value) { value.isolation = model::Isolation::SharedBridge; });
        const model::Session withFx = commit(edit);
        const auto wet = render(withFx, pool, 0.05);
        REQUIRE_MESSAGE(wet.warnings.empty(), (wet.warnings.empty() ? "" : wet.warnings.front()));
        auto* instance = pool.find(fx);
        REQUIRE(instance);
        CHECK(instance->bridgePid() > 0);
        if (sharedPid > 0) CHECK(instance->bridgePid() == sharedPid);
        sharedPid = instance->bridgePid();
        CHECK(peak(wet.audio.channels[0]) == 0.0);  // missing fx would pass dry audio through
        pool.prune(base);  // destroys the last shared instance after this plan is retired
        CHECK(pool.find(fx) == nullptr);
        if (cycle % 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
    // A file at another rate is resampled: the same ramp, 44.1 kHz frames
    // spaced at 48 kHz, the take offset still 0.1 s (4410 source frames).
    const model::Session slow = commit(t3);
    auto r3 = renderOffline(
        slow, pool, {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir, .media = &media});
    REQUIRE(r3);
    CHECK(r3->warnings.empty());
    CHECK(peak(r3->audio.channels[0], 36000) == 0.0);
    for (int k : {480, 1000, 11999}) {
        const auto i = static_cast<std::size_t>(24000 + k);
        const double src = (4410.0 + k * 44100.0 / 48000.0) / 48000.0;
        CHECK(double(r3->audio.channels[0][i]) == doctest::Approx(src * kCentre).epsilon(1e-3));
        CHECK(double(r3->audio.channels[1][i]) == doctest::Approx(-src * kCentre).epsilon(1e-3));
    }
    CHECK(media.size() == 2);
    auto again =
        renderOffline(slow, pool, {.startSeconds = 0, .endSeconds = 1.0, .projectDir = dir});
    REQUIRE(again);  // a fresh cache resamples to the same samples
    CHECK(again->audio.channels == r3->audio.channels);
    std::filesystem::remove_all(dir);
}

TEST_CASE("take playrate: varispeed resamples, the stretch keeps the pitch") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-media-stretch-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(48000));
    for (std::size_t i = 0; i < 48000; ++i) {
        const double s = 0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000.0);
        tone.channels[0][i] = static_cast<float>(s);
        tone.channels[1][i] = static_cast<float>(s);
    }
    REQUIRE(audio::writeWav(dir / "media" / "tone.wav", tone).ok());
    audio::AudioData longTone = tone;  // 4 s, for the variant size guard
    longTone.channels.assign(2, std::vector<float>(4 * 48000));
    REQUIRE(audio::writeWav(dir / "media" / "long.wav", longTone).ok());

    auto pool = makePool();
    MediaCache media;
    // An item of `beats` (0.5 s each at 120 BPM) starting at 0.5 s.
    auto build = [&](const char* file, double playrate, bool preservePitch, double pitch,
                     const char* mode, double beats, std::uint64_t rev = 1) {
        model::EditTxn t(model::Session::create(), rev);
        const Id tr = addTrack(t, "Audio");
        const Id md = t.create(model::MediaSource{file, "", "wav", 48000, 2, 48000});
        model::Take take;
        take.source = md;
        take.playrate = playrate;
        take.preservePitch = preservePitch;
        take.pitch = pitch;
        take.pitchMode = mode;
        const Id tk = t.create(take);
        model::Item item;
        item.track = tr;
        item.position = 1.0;
        item.length = beats;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
        return commit(t);
    };
    auto renderTake = [&](const model::Session& s, double seconds, MediaCache* cache = nullptr) {
        auto r = renderOffline(s, pool,
                               {.startSeconds = 0.0,
                                .endSeconds = seconds,
                                .projectDir = dir,
                                .media = cache ? cache : &media});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return std::move(*r);
    };
    constexpr double kTone = 0.5 * kCentre;

    // Varispeed: 2x halves the take's source time — its 1 s source fills a
    // 0.5 s item — and doubles the pitch. The old "time stretching is not
    // available yet" warning is gone.
    {
        auto r = renderTake(build("tone.wav", 2.0, false, 0.0, "resample", 1.0), 1.5);
        CHECK(r.warnings.empty());
        const auto& L = r.audio.channels[0];
        CHECK(peak(L, 0, 24000) == 0.0);
        CHECK(peak(L, 48000) == 0.0);
        CHECK(peak(L, 24000, 48000) == doctest::Approx(kTone).epsilon(0.02));
        CHECK(frequency(L, 27000, 45000) == doctest::Approx(880.0).epsilon(0.03));
        CHECK(media.size() == 1);
    }
    // Stretch: half the rate keeps the pitch and doubles the take's duration,
    // so 0.5 s of source fills the 1 s item.
    {
        auto r = renderTake(build("tone.wav", 0.5, true, 0.0, "signalsmith", 2.0), 2.0);
        CHECK(r.warnings.empty());
        const auto& L = r.audio.channels[0];
        CHECK(peak(L, 0, 24000) == 0.0);
        CHECK(peak(L, 72000) == 0.0);
        CHECK(peak(L, 24000, 72000) > 0.9 * kTone);
        CHECK(frequency(L, 34000, 69000) == doctest::Approx(440.0).epsilon(0.03));
        CHECK(media.size() == 2);
    }
    // The same stretch an octave up is another variant, not the cached one.
    {
        auto r = renderTake(build("tone.wav", 0.5, true, 12.0, "signalsmith", 2.0), 2.0);
        CHECK(r.warnings.empty());
        CHECK(frequency(r.audio.channels[0], 34000, 69000) == doctest::Approx(880.0).epsilon(0.03));
        CHECK(media.size() == 3);
    }
    // A pitch offset the resample cannot honour: warned, and the pitch stays.
    {
        auto r = renderTake(build("tone.wav", 1.0, true, 12.0, "resample", 1.0), 1.5);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].find("pitch_mode") != std::string::npos);
        CHECK(frequency(r.audio.channels[0], 27000, 45000) == doctest::Approx(440.0).epsilon(0.03));
    }
    // Without preserve_pitch the same offset is a further speed factor.
    {
        auto r = renderTake(build("tone.wav", 1.0, false, 12.0, "resample", 1.0), 1.5);
        CHECK(r.warnings.empty());
        CHECK(frequency(r.audio.channels[0], 27000, 45000) == doctest::Approx(880.0).epsilon(0.03));
    }
    // The stretched samples are the same in a cache that has to build them.
    {
        MediaCache fresh;
        auto a = renderTake(build("tone.wav", 0.5, true, 0.0, "signalsmith", 2.0), 2.0);
        auto b = renderTake(build("tone.wav", 0.5, true, 0.0, "signalsmith", 2.0), 2.0, &fresh);
        CHECK(a.audio.channels == b.audio.channels);
    }
    // A variant that would need an unreasonable amount of memory is refused,
    // not allocated (4 s at 1/100 speed and two octaves down).
    {
        auto r = renderTake(build("long.wav", 0.01, false, -48.0, "resample", 1.0), 1.5);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].find("memory") != std::string::npos);
        CHECK(peak(r.audio.channels[0]) == 0.0);
    }
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

TEST_CASE("the pool retires replaced instances until no plan can use them (review round 6)") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create(), 1);
    const Id tr = addTrack(t, "T");
    const Id fx = addFx(t, tr, kGain);
    const auto s1 = commit(t);
    pool.setGeneration(1);
    host::Instance* a = pool.get(fx, *s1.fx.find(fx), s1).value();

    // Another plugin under the same fx id: a new instance; the old one waits.
    model::EditTxn t2(s1, 2);
    t2.modify<model::FxInstance>(
        fx, [](model::FxInstance& f) { f.plugin.id = "com.suppleaudio.test.edge"; });
    const auto s2 = commit(t2);
    pool.setGeneration(2);
    host::Instance* b = pool.get(fx, *s2.fx.find(fx), s2).value();
    CHECK(b != a);
    CHECK(pool.retired() == 1);
    pool.prune(s2, 1);  // plan 1 (which used `a`) may still play
    CHECK(pool.retired() == 1);
    pool.prune(s2, 2);
    CHECK(pool.retired() == 0);

    // A plugin that cannot be found: the live instance is retired, not freed.
    model::EditTxn t3(s2, 3);
    t3.modify<model::FxInstance>(
        fx, [](model::FxInstance& f) { f.plugin.id = "com.suppleaudio.test.nope"; });
    const auto s3 = commit(t3);
    pool.setGeneration(3);
    CHECK_FALSE(pool.get(fx, *s3.fx.find(fx), s3));
    CHECK(pool.retired() == 1);
    CHECK(pool.find(fx) == nullptr);
    pool.prune(s3, 2);
    CHECK(pool.retired() == 1);
    pool.prune(s3, 3);
    CHECK(pool.retired() == 0);
}

TEST_CASE("a new state blob for a processing instance goes to a fresh instance (review round 6)") {
    auto pool = makePool();
    model::EditTxn t(model::Session::create(), 1);
    const Id tr = addTrack(t, "T");
    const Id fx = addFx(t, tr, "com.suppleaudio.test.edge");
    t.modify<model::FxInstance>(fx, [](model::FxInstance& f) { f.state = "sha256:one"; });
    t.putBlob("sha256:one", std::make_shared<const model::Blob>(model::Blob{{0, 1, 0, 0}}));
    const auto s1 = commit(t);
    host::Instance* a = pool.get(fx, *s1.fx.find(fx), s1).value();
    REQUIRE(a->activate(48000, 1, 256).ok());
    REQUIRE(a->startProcessing());  // as if a plan were playing it

    model::EditTxn t2(s1, 2);
    t2.modify<model::FxInstance>(fx, [](model::FxInstance& f) { f.state = "sha256:two"; });
    t2.putBlob("sha256:two", std::make_shared<const model::Blob>(model::Blob{{0, 2, 0, 0}}));
    const auto s2 = commit(t2);
    host::Instance* b = pool.get(fx, *s2.fx.find(fx), s2).value();
    CHECK(b != a);  // `a` keeps processing untouched until a plan stops it
    CHECK(pool.retired() == 1);
    CHECK(b->tail() == 2);  // the edge fixture reports its state bytes as its tail
    a->stopProcessing();

    // Not processing: the state is loaded in place.
    model::EditTxn t3(s2, 3);
    t3.modify<model::FxInstance>(fx, [](model::FxInstance& f) { f.state = "sha256:three"; });
    t3.putBlob("sha256:three", std::make_shared<const model::Blob>(model::Blob{{0, 3, 0, 0}}));
    const auto s3 = commit(t3);
    CHECK(pool.get(fx, *s3.fx.find(fx), s3).value() == b);
    CHECK(b->tail() == 3);
}

TEST_CASE("plugin parameter automation: sample-accurate steps, ramps in the curve domain") {
    auto pool = makePool();
    auto build = [](std::string param, std::vector<model::EnvPoint> points) {
        model::EditTxn t(model::Session::create(), 1);
        const Id tr = addTrack(t, "Lead");
        addFx(t, tr, kSine);
        const Id g = addFx(t, tr, kGain);
        addNote(t, tr, 0.0, 4.0);
        model::Envelope env;
        env.target = {g, std::move(param)};
        env.points = std::move(points);
        t.create(env);
        return commit(t);
    };
    using model::PointShape;
    // A step to -60 dB (off at min) at beat 1 = sample 24000: silence from that sample.
    {
        auto r = render(build("gain", {{0.0, 0.0, PointShape::Square, 0.0},
                                       {1.0, -60.0, PointShape::Square, 0.0}}),
                        pool, 1.0);
        CHECK(r.warnings.empty());
        CHECK(peak(r.audio.channels[0], 23800, 24000) > 0.05);
        CHECK(peak(r.audio.channels[0], 24000, 48000) == 0.0);
    }
    // The same by CLAP id, in CLAP values (the normalized position for stock plugins).
    {
        auto r = render(build("0", {{0.0, 60.0 / 72.0, PointShape::Square, 0.0},
                                    {1.0, 0.0, PointShape::Square, 0.0}}),
                        pool, 1.0);
        CHECK(r.warnings.empty());
        CHECK(peak(r.audio.channels[0], 23800, 24000) > 0.05);
        CHECK(peak(r.audio.channels[0], 24000, 48000) == 0.0);
    }
    // A linear ramp 0 → -24 dB over one second: -12 dB halfway.
    {
        auto r = render(build("gain", {{0.0, 0.0, PointShape::Linear, 0.0},
                                       {2.0, -24.0, PointShape::Linear, 0.0}}),
                        pool, 1.0);
        const double start = peak(r.audio.channels[0], 200, 1000);
        const double mid = peak(r.audio.channels[0], 23800, 24200);
        CHECK(mid / start == doctest::Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(0.03));
    }
    // An unknown parameter is reported, not fatal.
    {
        auto r = render(build("gian", {{0.0, 0.0, PointShape::Linear, 0.0}}), pool, 0.1);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].find("gian") != std::string::npos);
    }
}

TEST_CASE("plugins see the transport loop (CLAP loop flags)") {
    model::EditTxn t(model::Session::create(), 1);
    addTrack(t, "Track");
    const model::Session s = commit(t);
    auto pool = makePool();
    auto plan = detail::compile(s, pool, 0.0, 256).value();
    const host::Transport none = detail::transportAt(plan, 1000);
    CHECK_FALSE(none.loopActive);
    const LoopRange loop{48000, 96000, true};  // 1 s .. 2 s = beats 2 .. 4 at 120 BPM
    const host::Transport t2 = detail::transportAt(plan, 60000, &loop);
    CHECK(t2.loopActive);
    CHECK(t2.loopStartSeconds == doctest::Approx(1.0));
    CHECK(t2.loopEndSeconds == doctest::Approx(2.0));
    CHECK(t2.loopStartBeat == doctest::Approx(2.0));
    CHECK(t2.loopEndBeat == doctest::Approx(4.0));
    const LoopRange off{48000, 96000, false};
    CHECK_FALSE(detail::transportAt(plan, 60000, &off).loopActive);
}
