#include <doctest/doctest.h>
#include <supple/base/environment.h>

#include <supple/model/schemas.h>
#include <supple/model/session.h>
#include <cstdio>
#include <cstdlib>

using namespace supple;
using namespace supple::model;

namespace {

Id addTrack(EditTxn& t, std::string name, Id parent = kMaster) {
    Track tr;
    tr.name = std::move(name);
    tr.parent = parent;
    const Id id = t.create(tr);
    t.modify<Track>(parent, [&](Track& p) { p.children.push_back(id); });
    return id;
}

// A session with a bit of everything, for round trips.
Session richSession() {
    Session s = Session::create({.name = "Demo", .sampleRate = 48000});
    EditTxn t(s, 1);
    const Id drums = addTrack(t, "Drums");
    const Id kick = addTrack(t, "Kick", drums);
    const Id bass = addTrack(t, "Bass");
    t.modify<Track>(kick, [](Track& k) { k.volumeDb = -3.0; });
    MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 3840;
    src.sequence.notes = {{0, 480, 0, 36, 110, 0}, {960, 480, 9, 38, 90, 64}};
    src.sequence.controls = {{0, midi::ControlKind::CC, 0, 74, 64},
                             {480, midi::ControlKind::PitchBend, 0, 0, -8192}};
    src.sequence.sysex = {{0, {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}}};
    const Id mi = t.create(src);
    Take take;
    take.name = "kick loop";
    take.source = mi;
    const Id tk = t.create(take);
    Item item;
    item.track = kick;
    item.position = 0;
    item.length = 16;
    item.takes = {tk};
    item.activeTake = tk;
    item.fadeIn = {0.01, FadeShape::Cos, 0};
    t.create(item);
    FxInstance synth;
    synth.plugin = {"clap", "com.suppleaudio.probe-synth", "Probe Synth", "Supple Audio", "0.1.0"};
    synth.patch =
        json::parse(R"({"schema": "supple.probe-synth-patch/1.0", "params": {"cutoff": 800}})")
            .value();
    const Id fx = t.create(synth);
    FxInstance third;
    third.plugin = {"clap", "org.example.comp", "", "", ""};
    third.state = "sha256:abc";
    third.params = {{0, 0.5}, {7, 1.0}};
    const Id fx2 = t.create(third);
    t.putBlob("sha256:abc", std::make_shared<const Blob>(Blob{{1, 2, 3, 250}}));
    t.modify<Track>(bass, [&](Track& b) { b.chain = {fx, fx2}; });
    Route side;
    side.from.track = kick;
    side.to.track = bass;
    side.tap = Tap::PreFader;
    side.channels = {{1, 2}, {3, 4}};
    t.create(side);
    Envelope env;
    env.target = {bass, "volume"};
    env.points = {{0, -6, PointShape::Linear, 0}, {4, 0, PointShape::Slow, 0.2}};
    t.create(env);
    t.create(Marker{4.0, Timebase::Beats, "Verse", ""});
    t.create(Region{16.0, 32.0, Timebase::Beats, "Chorus", "#ff8800"});
    t.create(TempoPointObj{16.0, 140.0, true});
    t.create(TempoPointObj{32.0, 150.0, false});
    t.create(MeterObj{9, 3, 4});
    auto out = t.commit();
    REQUIRE_MESSAGE(out, (out ? std::string() : out.error().toString()));
    return std::move(*out);
}

}  // namespace

TEST_CASE("ids: canonical text form only") {
    CHECK(Id::parse("tr_0") == kMaster);
    CHECK(Id::parse("fx_12")->str() == "fx_12");
    CHECK(Id::parse("rj_4294967295").has_value());
    for (const char* bad :
         {"tr_01", "xx_1", "tr_", "tr_-1", "tr1", "TR_1", "tr_1x", "tr_4294967296"})
        CHECK_MESSAGE(!Id::parse(bad).has_value(), bad);
    CHECK(Id{Kind::Track, 2} < Id{Kind::Track, 10});
    CHECK(Id{Kind::Track, 99} < Id{Kind::Item, 1});
}

TEST_CASE("a new session: master, one tempo point, one meter, counters past them") {
    const Session s = Session::create();
    REQUIRE(s.tracks.find(kMaster));
    CHECK(s.tracks.find(kMaster)->name == "Master");
    CHECK(s.tempo.contains(Id{Kind::TempoPoint, 1}));
    CHECK(s.meters.contains(Id{Kind::Meter, 1}));
    CHECK(s.next[Kind::Track] == 1);
    CHECK(s.next[Kind::TempoPoint] == 2);
    CHECK(s.timeMap->secondsAt(4) == 2.0);
    CHECK(checkInvariants(s).ok());
}

TEST_CASE("transactions: new IDs, change lists, revisions, structural sharing") {
    const Session s0 = Session::create();
    EditTxn t(s0, 7);
    const Id a = addTrack(t, "A");
    const Id b = addTrack(t, "B");
    CHECK(a == Id{Kind::Track, 1});
    CHECK(b == Id{Kind::Track, 2});
    CHECK(t.get<Track>(a)->name == "A");  // reads see the transaction's edits
    const Id c = addTrack(t, "C");
    t.modify<Track>(kMaster, [&](Track& m) { m.children.erase(m.children.end() - 1); });
    t.remove<Track>(c);  // created and removed: no change recorded
    auto changes = t.changes();
    REQUIRE(changes.size() == 3);
    CHECK(changes[0] == Change{Change::Op::Update, kMaster});
    CHECK(changes[1] == Change{Change::Op::Add, a});
    CHECK(changes[2] == Change{Change::Op::Add, b});
    auto s1 = t.commit();
    REQUIRE(s1);
    CHECK(s1->version == 7);
    CHECK(s1->tracks.entry(a)->rev == 7);
    CHECK(s1->next[Kind::Track] == 4);       // tr_3 was used (then removed): never reused
    CHECK(s1->items.sameStorage(s0.items));  // untouched collections are shared
    CHECK_FALSE(s1->tracks.sameStorage(s0.tracks));
    CHECK(s0.tracks.size() == 1);  // the base is unchanged
    CHECK(t.commit().error().code == "model.txn");
}

TEST_CASE("invariants catch structural bugs") {
    const Session s0 = Session::create();
    {
        EditTxn t(s0, 1);
        Track orphan;
        orphan.parent = Id{Kind::Track, 42};
        t.create(orphan);
        auto r = t.commit();
        REQUIRE(!r);
        CHECK(r.error().code == "model.invariant");
    }
    {
        EditTxn t(s0, 1);
        const Id x = addTrack(t, "X");
        const Id y = addTrack(t, "Y");
        Route r1, r2;
        r1.from.track = x;
        r1.to.track = y;
        r2.from.track = y;
        r2.to.track = x;
        t.create(r1);
        t.create(r2);
        auto r = t.commit();
        REQUIRE(!r);
        CHECK(r.error().message.find("cycle") != std::string::npos);
    }
    {
        EditTxn t(s0, 1);
        FxInstance f;
        f.plugin.id = "x";
        t.create(f);  // in no chain
        auto r = t.commit();
        REQUIRE(!r);
        CHECK(r.error().path == "/fx/fx_1");
    }
    {
        EditTxn t(s0, 1);
        const Id tr = addTrack(t, "T");
        const Id tk = t.create(Take{});
        Item i1, i2;
        i1.track = i2.track = tr;
        i1.takes = i2.takes = {tk};
        i1.activeTake = i2.activeTake = tk;
        t.create(i1);
        t.create(i2);
        auto r = t.commit();
        REQUIRE(!r);
        CHECK(r.error().message.find("also belongs") != std::string::npos);
    }
    {
        EditTxn t(s0, 1);
        t.modify<TempoPointObj>(Id{Kind::TempoPoint, 1}, [](TempoPointObj& p) { p.beat = 1; });
        auto r = t.commit();
        REQUIRE(!r);  // no tempo point at beat 0
        CHECK(r.error().code == "model.invariant");
    }
}

TEST_CASE("project file: canonical, defaults omitted, byte-identical round trip") {
    const Session s = richSession();
    const std::string text = serializeProject(s);
    if (supple::environmentFlagEnabled("SUPPLE_DUMP_PROJECT")) std::fputs(text.c_str(), stdout);
    auto back = parseProject(text);
    REQUIRE_MESSAGE(back, (back ? std::string() : back.error().toString()));
    CHECK(serializeProject(*back) == text);
    CHECK(diff(s, *back).empty());
    CHECK(back->timeMap->bpmAt(24) == doctest::Approx(145.0));  // ramp 140 -> 150 over 16..32
    CHECK(back->timeMap->meterAt(10).numerator == 3);
    CHECK(back->blobs.at("sha256:abc")->data == std::vector<std::uint8_t>{1, 2, 3, 250});

    const json::Value doc = projectToJson(s);
    const json::Value* kick = doc.find("tracks")->find("tr_2");
    REQUIRE(kick);
    CHECK(kick->find("volume_db")->asDouble() == -3.0);
    CHECK(kick->find("pan") == nullptr);  // default: omitted
    CHECK(kick->find("channels") == nullptr);
    // Member order follows the spec.
    std::vector<std::string> order;
    for (const auto& m : doc.asObject()) order.push_back(m.key);
    CHECK(order == std::vector<std::string>{"schema", "project", "tempo", "rulers", "tracks",
                                            "items", "takes", "fx", "sources", "routes",
                                            "envelopes", "groups", "blobs"});
    // MIDI in compact rows, channels 1-based.
    const json::Value* notes =
        doc.find("sources")->find("midi")->find("mi_1")->find("sequence")->find("notes");
    CHECK(json::writeCompact(*notes) == "[[0,480,1,36,110,0],[960,480,10,38,90,64]]");
}

TEST_CASE("project file errors carry paths") {
    std::string text = serializeProject(richSession());
    auto bad = parseProject(
        std::string(text).replace(text.find("\"volume_db\": -3.0"), 17, "\"volume_db\": 99"));
    REQUIRE(!bad);
    CHECK(bad.error().path == "/tracks/tr_2/volume_db");
    auto wrongKey = json::parse(text).value();
    wrongKey["tracks"]["it_9"] = json::Value::object();
    auto r = projectFromJson(wrongKey);
    REQUIRE(!r);
    CHECK(r.error().path == "/tracks/it_9");
    auto future = json::parse(text).value();
    future["schema"] = "supple.project/9.0";
    CHECK(projectFromJson(future).error().code == "project.version");
}

TEST_CASE("diff: object-level, deterministic, field names") {
    const Session a = richSession();
    EditTxn t(a, 2);
    t.modify<Track>(Id{Kind::Track, 2}, [](Track& k) {
        k.volumeDb = -6.0;
        k.mute = true;
    });
    addTrack(t, "New");
    t.remove<Marker>(Id{Kind::Marker, 1});
    auto b = t.commit();
    REQUIRE(b);
    const auto d = diff(a, *b);
    REQUIRE(d.size() == 4);
    CHECK(d[0].op == Change::Op::Remove);  // markers come before tracks
    CHECK(d[0].id == Id{Kind::Marker, 1});
    CHECK(d[1].op == Change::Op::Update);  // the master's children
    CHECK(d[1].id == kMaster);
    CHECK(d[1].fields == std::vector<std::string>{"children"});
    CHECK(d[2].id == Id{Kind::Track, 2});
    CHECK(d[2].fields == std::vector<std::string>{"mute", "volume_db"});
    CHECK(d[3].op == Change::Op::Add);
}
