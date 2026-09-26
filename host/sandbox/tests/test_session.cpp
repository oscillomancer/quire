#include <doctest/doctest.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>

#include <supple/base/files.h>
#include <supple/sandbox/session.h>

using namespace supple;
using namespace supple::sandbox;
namespace fs = std::filesystem;

namespace {

const fs::path kFixture = FIXTURE_CLAP_PATH;
constexpr const char* kGain = "com.suppleaudio.test.gain";
constexpr const char* kSine = "com.suppleaudio.test.sine";

struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        path = fs::temp_directory_path() /
               (std::string("supple-sandbox-") + tag + "-" + std::to_string(::getpid()));
        fs::remove_all(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

std::unique_ptr<Session> openSession(const TempDir& dir) {
    SessionOptions o;
    o.workdir = dir.path;
    o.pluginPath = {kFixture};
    auto s = Session::open(o);
    REQUIRE(s);
    return std::move(*s);
}

Phrase twoNotes() {
    Phrase p;
    p.ppq = 960;
    p.notes = {{0, 960, 0, 69, 127}, {960, 960, 0, 76, 64}};  // two quarter notes
    return p;
}

}  // namespace

TEST_CASE("plugins: catalogue, lookup by id, case-insensitive id and path") {
    TempDir dir("plugins");
    auto s = openSession(dir);
    auto list = s->plugins();
    REQUIRE(list);
    CHECK(list->size() == 3);
    CHECK(s->findPlugin(kGain).value().name == "Test Gain");
    CHECK(s->findPlugin("COM.SUPPLEAUDIO.TEST.SINE").value().id == kSine);
    auto bad = s->findPlugin("com.suppleaudio.test.gian");
    REQUIRE(!bad);
    CHECK(bad.error().code == "sandbox.unknown_plugin");
    CHECK(bad.error().hint.find(kGain) != std::string::npos);
    auto byPath = s->findPlugin(kFixture.string());
    REQUIRE(!byPath);  // the fixture binary holds two plugins
    CHECK(byPath.error().code == "sandbox.ambiguous_plugin");
}

TEST_CASE("chain: instruments first, effects after, errors name the fix") {
    TempDir dir("chain");
    auto s = openSession(dir);
    auto g = s->load(kGain);
    REQUIRE(g);
    CHECK(*g == "in_1");
    auto i = s->load(kSine);  // an instrument goes first by default
    REQUIRE(i);
    CHECK(s->chain() == std::vector<std::string>{"in_2", "in_1"});
    auto second = s->load(kSine);
    REQUIRE(!second);
    CHECK(second.error().code == "sandbox.chain_order");
    auto front = s->load(kGain, 0);
    REQUIRE(!front);
    CHECK(front.error().path == "/position");
    auto past = s->load(kGain, 9);
    REQUIRE(!past);
    CHECK(past.error().code == "sandbox.position");
    auto g2 = s->load(kGain, 1);
    REQUIRE(g2);
    CHECK(*g2 == "in_3");  // the failed loads consumed no IDs
    CHECK(s->chain() == std::vector<std::string>{"in_2", "in_3", "in_1"});
    CHECK(s->move("in_1", 1).ok());
    CHECK(s->chain() == std::vector<std::string>{"in_2", "in_1", "in_3"});
    CHECK(s->move("in_2", 2).error().code == "sandbox.chain_order");
    CHECK(s->remove("in_1").ok());
    CHECK(s->chain() == std::vector<std::string>{"in_2", "in_3"});
    auto gone = s->instance("in_1");
    REQUIRE(!gone);
    CHECK(gone.error().code == "sandbox.unknown_instance");
    CHECK(gone.error().hint.find("in_3 (Test Gain)") != std::string::npos);
}

TEST_CASE("params: semantic keys with unit strings; errors carry arg paths") {
    TempDir dir("params");
    auto s = openSession(dir);
    const std::string g = s->load(kGain).value();
    json::Value values = json::Value::object();
    values["gain"] = "-6 dB";
    auto set = s->paramSet(g, values);
    REQUIRE(set);
    CHECK((*set)["gain"]["value"].asDouble() == doctest::Approx(-6.0));
    CHECK((*set)["gain"]["text"].asString().find("-6") != std::string::npos);
    auto all = s->paramGet(g);
    REQUIRE(all);
    CHECK(all->find("latency"));
    CHECK((*all)["latency"]["value"].asString() == "none");

    json::Value bad = json::Value::object();
    bad["gian"] = 1;
    auto e = s->paramSet(g, bad);
    REQUIRE(!e);
    CHECK(e.error().path == "/values/gian");
    json::Value range = json::Value::object();
    range["gain"] = "40 dB";
    auto r = s->paramSet(g, range);
    REQUIRE(!r);
    CHECK(r.error().path == "/values/gain");
    CHECK(s->paramGet(g).value()["gain"]["value"].asDouble() == doctest::Approx(-6.0));

    const std::string keys[] = {"gian"};
    auto unknown = s->paramGet(g, keys);
    REQUIRE(!unknown);
    CHECK(unknown.error().code == "sandbox.unknown_param");
    CHECK(unknown.error().path == "/keys/0");
    CHECK(unknown.error().hint.find("gain") != std::string::npos);
}

TEST_CASE("patches: merge reports the effective change; invalid patches change nothing") {
    TempDir dir("patches");
    auto s = openSession(dir);
    const std::string g = s->load(kGain).value();
    auto merge = json::parse(R"({"params": {"gain": -12}})");
    auto m = s->patchMerge(g, *merge);
    REQUIRE(m);
    REQUIRE(m->changed.find("params"));
    CHECK(m->changed.find("params")->size() == 1);
    CHECK((*m->changed.find("params")->find("gain")).asDouble() == -12.0);
    CHECK(!m->description.empty());
    auto again = s->patchMerge(g, *merge);
    REQUIRE(again);
    CHECK(json::writeCompact(again->changed) == "{}");  // idempotent

    const auto before = s->patchGet(g, false).value();
    auto bad = s->patchMerge(g, *json::parse(R"({"params": {"nope": 1}})"));
    REQUIRE(!bad);
    CHECK(bad.error().path == "/patch/params/nope");
    CHECK(s->patchGet(g, false).value() == before);

    auto full = s->patchSet(g, before);
    CHECK(full.ok());
    const std::string sine = s->load(kSine).value();
    auto ns = s->patchGet(sine, false);
    REQUIRE(!ns);
    CHECK(ns.error().code == "sandbox.not_semantic");
}

TEST_CASE("presets: save, list, load restores; names and plugins are checked") {
    TempDir dir("presets");
    auto s = openSession(dir);
    const std::string g = s->load(kGain).value();
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": -3}})")));
    const std::string tags[] = {"quiet"};
    auto path = s->presetSave(g, "Down 3", tags, "a little quieter");
    REQUIRE(path);
    CHECK(fs::exists(*path));
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": 6}})")));
    auto loaded = s->presetLoad(g, "Down 3");
    REQUIRE(loaded);
    CHECK(s->paramGet(g).value()["gain"]["value"].asDouble() == doctest::Approx(-3.0));
    auto list = s->presetList();
    REQUIRE(list);
    REQUIRE(list->size() == 1);
    CHECK(list->front().plugin == kGain);
    CHECK(list->front().tags == std::vector<std::string>{"quiet"});
    CHECK(s->presetList("com.other").value().empty());

    CHECK(s->presetSave(g, "../escape").error().code == "sandbox.preset_name");
    CHECK(s->presetSave(g, ".hidden").error().code == "sandbox.preset_name");
    auto missing = s->presetLoad(g, "Down 4");
    REQUIRE(!missing);
    CHECK(missing.error().code == "sandbox.unknown_preset");
    CHECK(missing.error().hint.find("Down 3") != std::string::npos);
    // The sine fixture has no state extension: saving says so.
    const std::string sine = s->load(kSine).value();
    CHECK(!s->presetSave(sine, "x").ok());
}

TEST_CASE("render: notes through instrument + effect, WAV written, deterministic" *
          doctest::description("rt")) {
    TempDir dir("render");
    auto s = openSession(dir);
    REQUIRE(s->load(kSine));
    const std::string g = s->load(kGain).value();
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": -6}})")));
    RenderInput in;
    in.phrase = twoNotes();
    in.name = "two notes";
    auto r = s->render(in);
    REQUIRE(r);
    const RenderRecord& rec = **r;
    CHECK(rec.id == "rd_1");
    CHECK(fs::exists(rec.path));
    // Two quarter notes at 120 BPM = 1 s, then a short silent tail.
    CHECK(rec.seconds >= 1.0);
    CHECK(rec.seconds < 1.3);
    CHECK_FALSE(rec.tailCapped);
    // Loudest note: velocity 127 → 0.25 amplitude, -6 dB.
    CHECK(rec.peakDbfs == doctest::Approx(20 * std::log10(0.25) - 6).epsilon(0.002));
    auto wav = audio::readAudio(rec.path);
    REQUIRE(wav);
    CHECK(static_cast<std::int64_t>(wav->frames()) == rec.frames);

    auto r2 = s->render(in);
    REQUIRE(r2);
    CHECK((*r2)->id == "rd_2");
    CHECK(readFile(rec.path).value() == readFile((*r2)->path).value());

    RenderInput tempo = in;
    tempo.tempo = 60.0;
    tempo.tailSeconds = 0.0;
    auto slow = s->render(tempo);
    REQUIRE(slow);
    CHECK((*slow)->frames == 96000);  // 2 s at 60 BPM, no tail

    RenderInput audioIn;
    audioIn.audio = audio::AudioData{};
    auto noInput = s->render(audioIn);
    REQUIRE(!noInput);
    CHECK(noInput.error().code == "render.no_input");
}

TEST_CASE("render: effect-only chain processes audio input") {
    TempDir dir("render-fx");
    auto s = openSession(dir);
    const std::string g = s->load(kGain).value();
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": -20}})")));
    RenderInput in;
    audio::AudioData a;
    a.sampleRate = 48000;
    a.channels.assign(2, std::vector<float>(4800, 0.5f));
    in.audio = a;
    in.tailSeconds = 0.0;
    auto r = s->render(in);
    REQUIRE(r);
    CHECK((*r)->frames == 4800);
    CHECK((*r)->peakDbfs == doctest::Approx(20 * std::log10(0.5) - 20).epsilon(0.001));
    RenderInput notes;
    notes.phrase = twoNotes();
    CHECK(s->render(notes).error().code == "render.not_instrument");
    a.sampleRate = 44100;
    in.audio = a;
    CHECK(s->render(in).error().code == "render.input_rate");
}

TEST_CASE("transactions: rollback restores chain, state and renders; IDs stay unique") {
    TempDir dir("txn");
    auto s = openSession(dir);
    const std::string sine = s->load(kSine).value();
    const std::string g = s->load(kGain).value();
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": -3}})")));
    RenderInput in;
    in.phrase = twoNotes();
    REQUIRE(s->render(in));

    REQUIRE(s->begin().ok());
    CHECK(s->begin().error().code == "sandbox.transaction_nested");
    REQUIRE(s->patchMerge(g, *json::parse(R"({"params": {"gain": -30}})")));
    const std::string g2 = s->load(kGain).value();
    REQUIRE(s->remove(sine).ok());
    auto r = s->render([] {
        RenderInput a;
        a.seconds = 0.1;
        return a;
    }());
    REQUIRE(r);
    const fs::path madeInTxn = (*r)->path;
    SessionSettings st = s->settings();
    st.tempo = 90;
    REQUIRE(s->setSettings(st).ok());
    s->rollback();

    CHECK(s->chain() == std::vector<std::string>{sine, g});
    CHECK(!s->instance(g2).ok());
    CHECK(s->paramGet(g).value()["gain"]["value"].asDouble() == doctest::Approx(-3.0));
    CHECK(s->renders().size() == 1);
    CHECK(!fs::exists(madeInTxn));
    CHECK(s->settings().tempo == 120.0);
    // The restored instrument still works.
    auto after = s->render(in);
    REQUIRE(after);
    CHECK((*after)->id == "rd_3");  // rd_2 was rolled back, never reused
    CHECK(s->load(kGain).value() == "in_4");

    REQUIRE(s->begin().ok());
    REQUIRE(s->remove(g).ok());
    s->commit();
    CHECK(!s->instance(g).ok());
}

TEST_CASE("settings: validation paths, summary, reset") {
    TempDir dir("settings");
    auto s = openSession(dir);
    SessionSettings bad;
    bad.sampleRate = 1000;
    bad.denominator = 3;
    auto e = s->setSettings(bad);
    REQUIRE(!e);
    REQUIRE(e.error().details.size() == 2);
    CHECK(e.error().details[0].path == "/sample_rate");
    CHECK(e.error().details[1].path == "/meter");
    REQUIRE(s->load(kSine));
    RenderInput in;
    in.phrase = twoNotes();
    REQUIRE(s->render(in));
    SessionSettings hi;
    hi.sampleRate = 96000;
    REQUIRE(s->setSettings(hi).ok());
    auto r = s->render(in);
    REQUIRE(r);
    CHECK((*r)->sampleRate == 96000);
    const auto sum = s->summary();
    CHECK(sum.find("settings")->find("meter")->asString() == "4/4");
    CHECK(sum.find("instances")->find("in_1"));
    CHECK(sum.find("renders")->size() == 2);
    const fs::path file = (*r)->path;
    s->reset();
    CHECK(s->chain().empty());
    CHECK(s->renders().empty());
    CHECK(!fs::exists(file));
    CHECK(s->settings().sampleRate == 48000);
}

TEST_CASE("phrase events: tick timing, offs before ons, zero-length notes") {
    Phrase p;
    p.ppq = 480;
    p.notes = {{0, 480, 0, 60, 100}, {480, 480, 0, 60, 100}, {960, 0, 9, 36, 127}};
    const auto ev = phraseEvents(p, 120.0, 48000.0);
    REQUIRE(ev.size() == 6);
    CHECK(ev[0].sample == 0);
    CHECK(ev[1].sample == 24000);
    CHECK(ev[1].event.kind == host::Event::Kind::NoteOff);  // first note ends…
    CHECK(ev[2].event.kind == host::Event::Kind::NoteOn);   // …before the next starts
    CHECK(ev[2].event.noteId == 1);
    CHECK(ev[3].sample == 48000);
    CHECK(ev[3].event.kind == host::Event::Kind::NoteOff);
    CHECK(ev[4].sample == 48000);
    CHECK(ev[4].event.kind == host::Event::Kind::NoteOn);
    CHECK(ev[5].sample == 48001);  // zero-length note lasts one sample
    CHECK(ev[5].event.kind == host::Event::Kind::NoteOff);
    CHECK(ev[5].event.channel == 9);
    CHECK(ticksToSamples(1, 960, 120.0, 48000.0) == 25);  // 25.0 exactly
    CHECK(ticksToSamples(7, 960, 97.0, 44100.0) == 199);  // 198.9…
}
