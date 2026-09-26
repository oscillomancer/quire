#include <doctest/doctest.h>
#include <supple/model/versions.h>

using namespace supple;
using namespace supple::model;

namespace {

// A toy command language for the replayer: track.add {name}, track.set
// {track, volume_db?, mute?}, track.remove {track}.
Result<Replayed> apply(const Session& base, const std::vector<Invocation>& inv,
                       std::uint64_t version) {
    EditTxn t(base, version);
    std::vector<Id> created;
    for (const auto& i : inv) {
        if (i.command == "track.add") {
            Track tr;
            tr.name = i.args.find("name")->asString();
            tr.parent = kMaster;
            const Id id = t.create(tr);
            t.modify<Track>(kMaster, [&](Track& m) { m.children.push_back(id); });
            created.push_back(id);
        } else if (i.command == "track.set" || i.command == "track.remove") {
            auto id = Id::parse(i.args.find("track")->asString());
            if (!id || !t.exists<Track>(*id))
                return Error("track.unknown", "no such track", "/track");
            if (i.command == "track.remove") {
                t.modify<Track>(kMaster, [&](Track& m) {
                    m.children.erase(std::find(m.children.begin(), m.children.end(), *id));
                });
                t.remove<Track>(*id);
            } else {
                t.modify<Track>(*id, [&](Track& tr) {
                    if (const auto* v = i.args.find("volume_db")) tr.volumeDb = v->asDouble();
                    if (const auto* m = i.args.find("mute")) tr.mute = m->asBool();
                });
            }
        } else {
            return Error("command.unknown", "unknown command " + i.command);
        }
    }
    auto changes = t.changes();
    auto s = t.commit();
    if (!s) return std::move(s).error();
    return Replayed{std::move(*s), std::move(changes), std::move(created)};
}

Invocation inv(std::string command, std::string_view args) {
    return {std::move(command), json::parse(args).value()};
}

// Runs invocations on the current head and commits them.
std::uint64_t run(VersionStore& vs, std::vector<Invocation> invs, std::string key = {}) {
    auto r = apply(*vs.head(), invs, vs.nextVersion());
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    TxnRecord txn{invs.front().command, "test", invs, std::move(key)};
    auto v = vs.commit(std::move(r->session), txn, std::move(r->changes), std::move(r->created));
    REQUIRE(v);
    return *v;
}

const Track* track(const VersionStore& vs, std::string_view branch, std::string_view id) {
    return vs.head(branch)->tracks.find(*Id::parse(id));
}

}  // namespace

TEST_CASE("undo and redo are linear per branch; a new edit clears redo") {
    VersionStore vs(Session::create());
    CHECK_FALSE(vs.canUndo());
    CHECK(vs.undo().error().code == "history.nothing_to_undo");
    run(vs, {inv("track.add", R"({"name": "A"})")});
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -3})")});
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -6})")});
    CHECK(track(vs, "main", "tr_1")->volumeDb == -6.0);
    REQUIRE(vs.undo().ok());
    REQUIRE(vs.undo().ok());
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);
    REQUIRE(vs.redo().ok());
    CHECK(track(vs, "main", "tr_1")->volumeDb == -3.0);
    run(vs, {inv("track.set", R"({"track": "tr_1", "mute": true})")});
    CHECK_FALSE(vs.canRedo());
    CHECK(vs.redo().error().code == "history.nothing_to_redo");
    CHECK(vs.history().size() == 4);  // mute, -3, add, initial
    REQUIRE(vs.undo().ok());
    REQUIRE(vs.undo().ok());
    REQUIRE(vs.undo().ok());
    CHECK(vs.head()->tracks.size() == 1);
    CHECK(vs.head()->next[Kind::Track] == 1);  // back to the initial state, counters too
}

TEST_CASE("coalescing: one gesture is one undo step") {
    VersionStore vs(Session::create());
    run(vs, {inv("track.add", R"({"name": "A"})")});
    for (int i = 1; i <= 5; ++i)
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -)" + std::to_string(i) + "}")},
            "fader:tr_1");
    CHECK(track(vs, "main", "tr_1")->volumeDb == -5.0);
    CHECK(vs.history().size() == 3);  // gesture, add, initial
    CHECK(vs.history().front()->txn.invocations.size() == 5);
    REQUIRE(vs.undo().ok());
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);
}

TEST_CASE("branches: isolated heads, fast-forward merge") {
    VersionStore vs(Session::create());
    run(vs, {inv("track.add", R"({"name": "A"})")});
    REQUIRE(vs.createBranch("agent/claude/darker", "main").ok());
    CHECK(vs.createBranch("agent/claude/darker", "main").error().code == "branch.exists");
    REQUIRE(vs.switchBranch("agent/claude/darker").ok());
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -9})")});
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);  // main is untouched
    CHECK_FALSE(vs.canRedo());
    REQUIRE(vs.undo().ok());
    CHECK_FALSE(vs.canUndo());  // undo stops where the branch began
    REQUIRE(vs.redo().ok());
    auto m = vs.merge("agent/claude/darker", "main", apply);
    CHECK(m.conflicts.empty());
    CHECK(m.fastForward);
    CHECK(track(vs, "main", "tr_1")->volumeDb == -9.0);
    CHECK(vs.merge("agent/claude/darker", "main", apply).upToDate);
    CHECK(vs.deleteBranch("main").error().code == "branch.protected");
    CHECK(vs.deleteBranch("agent/claude/darker").error().code == "branch.current");
    REQUIRE(vs.switchBranch("main").ok());
    CHECK(vs.deleteBranch("agent/claude/darker").ok());
    CHECK(vs.switchBranch("nope").error().code == "branch.unknown");
}

TEST_CASE("merge by replay remaps IDs created on both sides") {
    VersionStore vs(Session::create());
    REQUIRE(vs.createBranch("topic", "main").ok());
    run(vs, {inv("track.add", R"({"name": "Main track"})")});  // tr_1 on main
    REQUIRE(vs.switchBranch("topic").ok());
    run(vs, {inv("track.add", R"({"name": "Branch track"})")});  // tr_1 on the branch too
    run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -3})")});
    auto m = vs.merge("topic", "main", apply);
    REQUIRE_MESSAGE(m.conflicts.empty(), (m.conflicts.empty() ? "" : m.conflicts[0].message));
    CHECK_FALSE(m.fastForward);
    CHECK(m.remap.at(*Id::parse("tr_1")) == *Id::parse("tr_2"));
    CHECK(track(vs, "main", "tr_1")->name == "Main track");
    CHECK(track(vs, "main", "tr_1")->volumeDb == 0.0);
    CHECK(track(vs, "main", "tr_2")->name == "Branch track");
    CHECK(track(vs, "main", "tr_2")->volumeDb == -3.0);  // the edit followed the remap
    CHECK(vs.head("main")->tracks.find(kMaster)->children.size() == 2);  // no false conflict
    CHECK(vs.merge("topic", "main", apply).upToDate);
    REQUIRE(vs.switchBranch("main").ok());
    REQUIRE(vs.undo().ok());  // undo steps back through the replayed transactions
    CHECK(track(vs, "main", "tr_2")->volumeDb == 0.0);
}

TEST_CASE("merge conflicts: same field, removed object, failed replay; target untouched") {
    auto setup = [] {
        VersionStore vs(Session::create());
        run(vs, {inv("track.add", R"({"name": "A"})")});
        REQUIRE(vs.createBranch("topic", "main").ok());
        return vs;
    };
    {
        VersionStore vs = setup();
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -1})")});
        const auto headBefore = vs.headVersion("main");
        REQUIRE(vs.switchBranch("topic").ok());
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -2})")});
        auto m = vs.merge("topic", "main", apply);
        REQUIRE(m.conflicts.size() == 1);
        CHECK(m.conflicts[0].kind == Conflict::Kind::ConcurrentEdit);
        CHECK(m.conflicts[0].object == Id::parse("tr_1"));
        CHECK(m.conflicts[0].message.find("volume_db") != std::string::npos);
        CHECK(vs.headVersion("main") == headBefore);
    }
    {
        VersionStore vs = setup();  // different fields of one object merge cleanly
        run(vs, {inv("track.set", R"({"track": "tr_1", "mute": true})")});
        REQUIRE(vs.switchBranch("topic").ok());
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -2})")});
        auto m = vs.merge("topic", "main", apply);
        CHECK(m.conflicts.empty());
        CHECK(track(vs, "main", "tr_1")->mute);
        CHECK(track(vs, "main", "tr_1")->volumeDb == -2.0);
    }
    {
        VersionStore vs = setup();
        run(vs, {inv("track.remove", R"({"track": "tr_1"})")});
        REQUIRE(vs.switchBranch("topic").ok());
        run(vs, {inv("track.set", R"({"track": "tr_1", "volume_db": -2})")});
        auto m = vs.merge("topic", "main", apply);
        REQUIRE(m.conflicts.size() == 1);
        CHECK(m.conflicts[0].message.find("removed") != std::string::npos);
    }
    {
        VersionStore vs = setup();
        run(vs, {inv("track.add", R"({"name": "B"})")});
        REQUIRE(vs.switchBranch("topic").ok());
        run(vs, {inv("track.add", R"({"name": "C"})")});
        auto failing = [](const Session&, const std::vector<Invocation>&,
                          std::uint64_t) -> Result<Replayed> {
            return Error("render.failed", "replay refused");
        };
        auto m = vs.merge("topic", "main", failing);
        REQUIRE(m.conflicts.size() == 1);
        CHECK(m.conflicts[0].kind == Conflict::Kind::Invalid);
        CHECK(track(vs, "main", "tr_2")->name == "B");
    }
}

TEST_CASE("remapIds rewrites exact ID strings anywhere") {
    const std::map<Id, Id> remap{{*Id::parse("tr_1"), *Id::parse("tr_7")}};
    auto v =
        json::parse(R"({"track": "tr_1", "list": ["tr_1", "tr_10", "x tr_1"], "n": 1})").value();
    CHECK(json::writeCompact(remapIds(v, remap)) ==
          R"({"track":"tr_7","list":["tr_7","tr_10","x tr_1"],"n":1})");
}
