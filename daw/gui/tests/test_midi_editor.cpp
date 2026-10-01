// The MIDI editor driven like a user: pointer and keys through the root, the
// notation panel typed into, the session read back through commands.
#include <doctest/doctest.h>
#include <unistd.h>

#include <SDL3/SDL_keycode.h>

#include <filesystem>
#include <thread>

#include "app_actions.h"
#include "history_panel.h"
#include "midi_editor.h"
#include "mixer.h"
#include "routing_matrix.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/ui/controls.h"
#include "supple/ui/text_area.h"

using namespace supple;

namespace {

const cmd::Origin kTest{cmd::Origin::Type::Ui, "test", {}};

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    gui::UiState ui;
    std::unique_ptr<ui::Root> root;
    std::unique_ptr<gui::AppActions> actions;
    std::unique_ptr<gui::MidiEditor> editor;
    std::string item;
    bool ok = false;

    Fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-midi-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "config");
        ::setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"midi-test", 1100, 560, true});
        if (!w) return;
        window = std::move(*w);
        auto f = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        if (!f) return;
        fonts = *f;
        engine::AppOptions options;
        options.workdir = dir;
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        root = std::make_unique<ui::Root>(*window, fonts, ui::Theme::dark());
        actions = std::make_unique<gui::AppActions>(registry, *dispatcher, *app, ui,
                                                    actions::Keymap::defaultFile("quire-test"));
        editor = std::make_unique<gui::MidiEditor>(*root, *actions, *dispatcher, *app, ui);
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->emplace<ui::Node>("arrange").layout.height = ui::SizeRule::fraction();
        frame->add(editor->build());
        root->setContent(std::move(frame));
        auto t = exec("track.add", R"({"name":"Keys"})");
        auto it =
            exec("item.add", R"({"track":")" + t["track"].asString() +
                                 R"(","position":4,"notation":"voice v; 1: C4:q E4:q G4:q C5:q"})");
        item = it["item"].asString();
        root->frame(false);
        ok = true;
    }
    ~Fixture() {
        editor.reset();
        actions.reset();
        root.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    json::Value exec(const std::string& command, const std::string& args) {
        auto r = dispatcher->execute({command, json::parse(args).value(), kTest});
        INFO(command << ": " << (r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    }
    json::Value notes() { return exec("midi.notes", R"({"item":")" + item + R"("})")["notes"]; }
    void frame() {
        editor->sync();
        root->frame(false);
    }
    // Pointer input in window coordinates, the same path as SDL events.
    void pointer(ui::PointerEvent::Type type, ui::Point p, int clicks = 1, unsigned mods = 0) {
        ui::PointerEvent e;
        e.type = type;
        e.pos = p;
        e.button = 1;
        e.clicks = clicks;
        e.mods = mods;
        root->pointer(e);
        frame();
    }
    void drag(ui::Point from, ui::Point to, unsigned mods = 0) {
        pointer(ui::PointerEvent::Type::Down, from, 1, mods);
        const ui::Point mid{(from.x + to.x) / 2, (from.y + to.y) / 2};
        pointer(ui::PointerEvent::Type::Move, mid, 1, mods);
        pointer(ui::PointerEvent::Type::Move, to, 1, mods);
        pointer(ui::PointerEvent::Type::Up, to, 1, mods);
    }
    // Where a note's middle is drawn, from the roll's geometry.
    ui::Point at(double itemBeat, int pitch) const {
        const auto& s = editor->state();
        const ui::Rect b = editor->roll()->bounds();
        return {b.x + static_cast<float>((itemBeat - s.scrollBeat) * s.pixelsPerBeat),
                b.y + static_cast<float>(s.topPitch - pitch) * s.rowHeight + s.rowHeight / 2};
    }
    void snapshot(const char* name) {
        const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS");
        if (!out) return;
        root->damageAll();
        root->frame(false);
        (void)gui::writePng(std::filesystem::path(out) / name, root->readBack());
    }
};

}  // namespace

TEST_CASE("MIDI editor: open, draw, select, move, resize, transpose, delete, velocity") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    REQUIRE(f.editor->open(f.item));
    f.frame();
    auto& s = f.editor->state();
    REQUIRE(s.notes.size() == 4);
    CHECK(s.itemPosition == doctest::Approx(4.0));
    CHECK(f.editor->roll()->bounds().w > 300);
    f.snapshot("midi-editor-open.png");

    // Double-click empty space: a note of the grid length, snapped.
    s.grid = 0.5;
    s.lastLength = 0.5;
    const auto v0 = f.app->head().version;
    f.pointer(ui::PointerEvent::Type::Down, f.at(2.6, 65), 2);
    f.pointer(ui::PointerEvent::Type::Up, f.at(2.6, 65));
    auto n = f.notes();
    REQUIRE(n.size() == 5);
    CHECK(f.app->head().version == v0 + 1);
    bool found = false;
    for (const auto& x : n.asArray())
        if (x.find("pitch")->asInt() == 65) {
            found = true;
            CHECK(x.find("start")->asDouble() == doctest::Approx(2.5));
            CHECK(x.find("length")->asDouble() == doctest::Approx(0.5));
        }
    CHECK(found);
    REQUIRE(s.selected.size() == 1);  // the new note is selected

    // Drag E4 (beat 1) one beat later and up a tone: snapped, one version.
    const auto v1 = f.app->head().version;
    f.drag(f.at(1.3, 64), {f.at(2.3, 64).x, f.at(2.3, 66).y});
    n = f.notes();
    CHECK(f.app->head().version == v1 + 1);
    found = false;
    for (const auto& x : n.asArray())
        if (x.find("pitch")->asInt() == 66) {
            found = true;
            CHECK(x.find("start")->asDouble() == doctest::Approx(2.0));
        }
    CHECK(found);

    // Resize the end of C4 (beat 0, length 1) by half a beat.
    const ui::Point end = f.at(0.98, 60);
    f.drag(end, {end.x + static_cast<float>(0.5 * s.pixelsPerBeat), end.y});
    CHECK(f.notes().at(0).find("length")->asDouble() == doctest::Approx(1.5));

    // Keys: Up transposes the selection, Delete removes it.
    f.pointer(ui::PointerEvent::Type::Down, f.at(0.5, 60));
    f.pointer(ui::PointerEvent::Type::Up, f.at(0.5, 60));
    REQUIRE(s.selected.size() == 1);
    f.root->key(SDLK_UP, 0);
    f.frame();
    CHECK(f.notes().at(0).find("pitch")->asInt() == 61);
    f.root->key(SDLK_UP, ui::kShift);
    f.frame();
    CHECK(f.notes().at(0).find("pitch")->asInt() == 73);
    const std::size_t before = f.notes().size();
    f.root->key(SDLK_DELETE, 0);
    f.frame();
    CHECK(f.notes().size() == before - 1);

    // Velocity: drag a stem in the lane to the top.
    f.root->key(SDLK_A, ui::kCtrl);
    f.frame();
    f.pointer(ui::PointerEvent::Type::Down, f.at(0.0, 60));  // deselect by clicking empty space
    f.pointer(ui::PointerEvent::Type::Up, f.at(0.0, 60));
    const ui::Rect lane = f.root->find("midi.velocity")->bounds();
    const auto first = f.notes().at(0);
    const float stemX = f.at(first.find("start")->asDouble(), 60).x + 1;
    f.drag({stemX, lane.bottom() - 10}, {stemX, lane.y + 2});
    CHECK(f.notes().at(0).find("velocity")->asInt() >= 120);

    // Quantize everything to the beat after nudging a note off the grid.
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"move","notes":[0],"beats":0.1}]})");
    f.frame();
    s.grid = 1;
    f.editor->quantize();
    const json::Value quantized = f.notes();
    for (const auto& x : quantized.asArray()) {
        const double start = x.find("start")->asDouble() + 4;  // project beats
        CHECK(std::abs(start - std::round(start * 2) / 2) < 1e-6);
    }
    f.snapshot("midi-editor-edited.png");
}

TEST_CASE("MIDI editor: the notation panel compiles live, writes back, and follows the roll") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    ui::TextArea* text = f.editor->notation();
    REQUIRE(text);
    CHECK(text->text().find("C4") != std::string::npos);

    // Type a new phrase: after the pause it compiles and is written.
    f.root->focus(text);
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("voice v; 1: D4:h F4:h");
    f.frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    f.frame();
    auto n = f.notes();
    REQUIRE(n.size() == 2);
    CHECK(n.at(0).find("pitch")->asInt() == 62);
    CHECK(f.editor->state().notes.size() == 2);  // the roll followed

    // A mistake: located diagnostics, nothing written.
    const auto v = f.app->head().version;
    f.root->key(SDLK_END, ui::kCtrl);
    f.root->text(" Q9:zz");
    f.frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    f.frame();
    CHECK(f.app->head().version == v);
    REQUIRE_FALSE(text->marks().empty());
    CHECK(text->marks().front().line == 1);
    f.snapshot("midi-editor-diagnostic.png");

    // Fix it; the typing session is one undo step.
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("voice v; 1: A3:w");
    f.frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    f.frame();
    CHECK(text->marks().empty());
    CHECK(f.notes().size() == 1);

    // An edit from elsewhere (an agent) reaches both views when the text is not being typed in.
    f.root->focus(f.editor->roll());
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"add","pitch":72,"start":1,"length":1}]})");
    f.frame();
    CHECK(f.editor->state().notes.size() == 2);
    CHECK(text->text().find("C5") != std::string::npos);

    // The item goes away: the editor closes.
    f.exec("item.remove", R"({"item":")" + f.item + R"("})");
    f.frame();
    CHECK_FALSE(f.editor->isOpen());
}

TEST_CASE("routing matrix: rules shown, a click sends, the send edits and goes") {
    Fixture f;
    if (!f.ok) return;
    const std::string drums =
        f.exec("track.add", R"({"type":"buss","name":"Drums"})")["track"].asString();
    const std::string verb =
        f.exec("track.add", R"({"type":"buss","name":"Verb"})")["track"].asString();
    f.exec("route.add", R"({"from":")" + verb + R"(","to":")" + drums + R"("})");
    gui::RoutingMatrix matrix(*f.root, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    f.root->frame(false);
    REQUIRE(matrix.isOpen());
    const auto& m = matrix.model();
    auto row = [&](const std::string& id) {
        for (std::size_t i = 0; i < m.sources.size(); ++i)
            if (m.sources[i].id == id) return i;
        FAIL("no source " << id);
        return std::size_t{0};
    };
    auto col = [&](const std::string& id) {
        for (std::size_t i = 0; i < m.destinations.size(); ++i)
            if (m.destinations[i].id == id) return i;
        FAIL("no destination " << id);
        return std::size_t{0};
    };
    std::string keys;
    for (const auto& s : m.sources)
        if (s.name == "Keys") keys = s.id;
    REQUIRE_FALSE(keys.empty());
    // Rules: a channel takes no strip audio; the master is terminal; Drums -> Verb feeds back.
    CHECK_FALSE(m.blocked(row(drums), col(keys)).empty());
    CHECK_FALSE(m.blocked(row("tr_0"), col(drums)).empty());
    CHECK(m.blocked(row(drums), col(verb)).find("feed back") != std::string::npos);
    CHECK(m.blocked(row(keys), col(drums)).empty());
    // Hovering a blocked cell says why.
    auto* grid = matrix.grid();
    REQUIRE(grid);
    f.snapshot("routing-open.png");
    // A click on an allowed cell sends (one version) and selects the new send.
    const auto v = f.app->head().version;
    REQUIRE(matrix.clickCell(row(keys), col(drums)));
    CHECK(f.app->head().version == v + 1);
    REQUIRE(matrix.selectedSend());
    CHECK(m.sendsAt(row(keys), col(drums)).size() == 1);
    // A blocked click changes nothing.
    CHECK_FALSE(matrix.clickCell(row(drums), col(verb)));
    CHECK(f.app->head().version == v + 1);
    // Edit the send: level with units, pre-fader, mute; then remove it.
    f.root->click("routing.volume");
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("-6 dB");
    f.root->key(SDLK_RETURN, 0);
    CHECK(m.sendsAt(row(keys), col(drums)).front()->volumeDb == doctest::Approx(-6.0));
    REQUIRE(f.root->click("routing.tap.pre"));
    CHECK(m.sendsAt(row(keys), col(drums)).front()->tap == "pre_fader");
    REQUIRE(f.root->click("routing.mute"));
    CHECK(m.sendsAt(row(keys), col(drums)).front()->mute);
    f.snapshot("routing-send.png");
    REQUIRE(f.root->click("routing.remove"));
    CHECK(m.sendsAt(row(keys), col(drums)).empty());
    // Changes from elsewhere arrive on sync.
    f.exec("route.add", R"({"from":")" + keys + R"(","to":")" + verb + R"("})");
    matrix.sync();
    CHECK(m.sendsAt(row(keys), col(verb)).size() == 1);
    // The parent's cell toggles the parent send.
    REQUIRE(matrix.clickCell(row(keys), col("tr_0")));
    CHECK_FALSE(f.app->head().tracks.find(*model::Id::parse(keys))->parentSend.enabled);
    REQUIRE(matrix.clickCell(row(keys), col("tr_0")));
    CHECK(f.app->head().tracks.find(*model::Id::parse(keys))->parentSend.enabled);
    REQUIRE(f.root->click("routing.close"));
    CHECK_FALSE(matrix.isOpen());
}

TEST_CASE("mixer: grouped strips, a fader drag is one undo step, toggles, pan, Console 9, meters") {
    Fixture f;
    if (!f.ok) return;
    const std::string buss =
        f.exec("track.add", R"({"type":"buss","name":"Drums"})")["track"].asString();
    std::vector<gui::StripReading> readings;
    gui::MeterFeed feed([&] { return readings; });
    gui::Mixer mixer(*f.root, *f.actions, *f.dispatcher, *f.app, feed);
    // Swap the editor's dock for the mixer's in the fixture's window.
    auto* window = f.root->content();
    window->add(mixer.build());
    f.editor->close();
    mixer.setVisible(true);
    f.root->frame(false);
    const auto& strips = mixer.strips();
    REQUIRE(strips.size() == 3);
    CHECK(strips[0].type == "channel");  // channels, busses, masters
    CHECK(strips[1].track == buss);
    CHECK(strips[2].track == "tr_0");
    CHECK(strips[0].arm != nullptr);
    CHECK(strips[1].arm == nullptr);  // only channels arm
    const std::string keys = strips[0].track;
    auto track = [&](const std::string& id) {
        return f.app->head().tracks.find(*model::Id::parse(id));
    };

    // A fader drag: many moves, one undo step; a second drag is another.
    auto steps = [&] { return f.exec("history.list", "{}")["versions"].size(); };
    const auto s0 = steps();
    const ui::Rect fb = strips[0].fader->bounds();
    REQUIRE(fb.h > 60);
    const ui::Point cap{fb.x + fb.w / 2,
                        fb.bottom() - 8 - static_cast<float>(ui::faderPosition(0)) * (fb.h - 16)};
    f.pointer(ui::PointerEvent::Type::Down, cap);
    for (int i = 1; i <= 10; ++i)
        f.pointer(ui::PointerEvent::Type::Move, {cap.x, cap.y + static_cast<float>(i) * 3});
    f.pointer(ui::PointerEvent::Type::Up, {cap.x, cap.y + 30});
    mixer.sync();
    CHECK(track(keys)->volumeDb < -3);
    CHECK(steps() == s0 + 1);
    const double dragged = track(keys)->volumeDb;
    const ui::Point cap2{cap.x, cap.y + 30};
    f.pointer(ui::PointerEvent::Type::Down, cap2);
    for (int i = 1; i <= 5; ++i)
        f.pointer(ui::PointerEvent::Type::Move, {cap2.x, cap2.y + static_cast<float>(i) * 3});
    f.pointer(ui::PointerEvent::Type::Up, {cap2.x, cap2.y + 15});
    CHECK(steps() == s0 + 2);
    f.exec("history.undo", "{}");
    CHECK(track(keys)->volumeDb == doctest::Approx(dragged));  // only the second drag undone
    mixer.sync();
    // Double-click: unity, its own step.
    f.pointer(ui::PointerEvent::Type::Down, cap2, 2);
    f.pointer(ui::PointerEvent::Type::Up, cap2);
    CHECK(track(keys)->volumeDb == 0.0);
    // Toggles.
    REQUIRE(f.root->click("mixer.mute." + keys));
    CHECK(track(keys)->mute);
    REQUIRE(f.root->click("mixer.solo." + buss));
    CHECK(track(buss)->solo == model::Solo::On);
    REQUIRE(f.root->click("mixer.arm." + keys));
    CHECK(track(keys)->record.armed);
    REQUIRE(f.root->click("mixer.monitor." + keys));
    CHECK(track(keys)->record.monitor == model::Monitor::On);
    // Pan drag right.
    const ui::Rect pb = strips[0].pan->bounds();
    f.drag({pb.x + pb.w / 2, pb.y + 8}, {pb.right() - 2, pb.y + 8});
    CHECK(track(keys)->pan > 50);
    // Console 9 is a project toggle.
    const bool c9 = f.app->head().project->console9;
    REQUIRE(f.root->click("mixer.console9"));
    CHECK(f.app->head().project->console9 == !c9);
    // Changes from elsewhere reach the strips; a new track adds a strip.
    f.exec("track.set", R"({"track":")" + keys + R"(","volume_db":-12})");
    mixer.sync();
    CHECK(strips[0].fader->value() == doctest::Approx(-12.0));
    f.exec("track.add", R"({"name":"Bass"})");
    mixer.sync();
    CHECK(mixer.strips().size() == 4);
    // Meters: readings land on the strip's meter.
    readings = {{keys, {0.5f, 0.25f}, {0.3f, 0.1f}}};
    feed.poll();
    CHECK(mixer.strips()[0].meter->shownPeakDb(0) ==
          doctest::Approx(20 * std::log10(0.5)).epsilon(0.02));
    f.snapshot("mixer.png");
}

TEST_CASE(
    "history dock: versions, jump back, redo, branches, the command log with agents attributed") {
    Fixture f;
    if (!f.ok) return;
    gui::HistoryPanel history(*f.root, *f.actions, *f.dispatcher, *f.app);
    f.editor->close();
    {  // the app's layout: the dock is a full-height column on the right
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Row;
        frame->emplace<ui::Node>("arrange").layout.width = ui::SizeRule::fraction();
        frame->add(history.build());
        f.root->setContent(std::move(frame));
    }
    history.setVisible(true);
    const cmd::Origin agent{cmd::Origin::Type::Agent, "panel", "model-x"};
    f.exec("track.add", R"({"name":"A"})");
    f.exec("track.add", R"({"name":"B"})");
    REQUIRE(f.dispatcher->execute({"track.add", json::parse(R"({"name":"C"})").value(), agent}));
    history.sync();
    f.root->frame(false);
    REQUIRE(history.versions().size() >= 4);
    CHECK(history.versions().front().origin.rfind("agent:", 0) == 0);  // the head, by the agent
    // The log: newest last, the agent's command attributed.
    REQUIRE(!history.entries().empty());
    CHECK(history.entries().back().origin == "agent:panel/model-x");
    CHECK(history.entries().back().command == "track.add");
    f.snapshot("history.png");
    // Jump back two versions, then redo one.
    const std::size_t tracks = f.app->head().tracks.size();
    REQUIRE(history.jumpTo(2));
    CHECK(f.app->head().tracks.size() == tracks - 2);
    REQUIRE(f.root->click("history.redo"));
    CHECK(f.app->head().tracks.size() == tracks - 1);
    // Branches: create, edit there, back to main.
    REQUIRE(history.createBranch("idea"));
    history.sync();
    CHECK(std::find(history.branches().begin(), history.branches().end(), "idea") !=
          history.branches().end());
    f.exec("track.add", R"({"name":"Idea"})");
    REQUIRE(history.switchBranch("main"));
    CHECK(f.app->head().tracks.size() == tracks - 1);
    history.sync();
    CHECK(history.entries().back().command == "branch.switch");
}
