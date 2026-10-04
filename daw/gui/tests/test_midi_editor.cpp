// The MIDI editor driven like a user: pointer and keys through the root, the
// notation panel typed into, the session read back through commands.
#include <doctest/doctest.h>
#include <unistd.h>
#include "product_fixture.h"

#include <SDL3/SDL_keycode.h>

#include <filesystem>
#include <thread>

#include "app_actions.h"
#include "history_panel.h"
#include "midi_editor.h"
#include "midi_event_list.h"
#include "mixer.h"
#include "routing_matrix.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/ui/controls.h"
#include "supple/ui/text_area.h"
#include "supple/ui/widgets.h"

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
        actions = std::make_unique<gui::AppActions>(
            registry, *dispatcher, *app, ui, actions::Keymap::defaultFile(gui::test::slug()));
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

TEST_CASE("MIDI editor: an empty roll can marquee without a note anchor") {
    Fixture f;
    if (!f.ok) return;
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"remove","notes":[0,1,2,3]}]})");
    REQUIRE(f.editor->open(f.item));
    f.frame();
    const auto version = f.app->head().version;
    f.drag(f.at(0.5, 60), f.at(2, 64));
    CHECK(f.editor->state().notes.empty());
    CHECK(f.editor->state().selected.empty());
    CHECK(f.app->head().version == version);
}

TEST_CASE("MIDI editor: selected notes painted on top win overlapping hit tests") {
    Fixture f;
    if (!f.ok) return;
    f.exec("midi.edit",
           R"({"item":")" + f.item +
               R"(","ops":[{"op":"add","pitch":60,"start":0,"length":1,"velocity":40}]})");
    REQUIRE(f.editor->open(f.item));
    f.frame();
    auto& state = f.editor->state();
    REQUIRE(state.notes.size() == 5);
    state.selected = {0};
    f.frame();
    f.pointer(ui::PointerEvent::Type::Down, f.at(0.4, 60));
    CHECK(state.selected == std::set<std::size_t>{0});
    f.pointer(ui::PointerEvent::Type::Up, f.at(0.4, 60));
    CHECK(state.selected == std::set<std::size_t>{0});
}

TEST_CASE("MIDI editor: external note removal cancels a captured drag") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    const ui::Point start = f.at(3.5, 72);
    f.pointer(ui::PointerEvent::Type::Down, start);
    REQUIRE(f.editor->state().drag == gui::RollState::Drag::Move);
    f.exec("midi.edit", R"({"item":")" + f.item + R"(","ops":[{"op":"remove","notes":[3]}]})");
    f.frame();
    CHECK(f.editor->state().drag == gui::RollState::Drag::None);
    const auto version = f.app->head().version;
    f.pointer(ui::PointerEvent::Type::Move, {start.x + 20, start.y - 12});
    f.pointer(ui::PointerEvent::Type::Up, {start.x + 20, start.y - 12});
    CHECK(f.app->head().version == version);
    CHECK(f.notes().size() == 3);
}

TEST_CASE("MIDI editor: closing cancels velocity and CC previews before reopening") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    SUBCASE("velocity") {
        const ui::Rect lane = f.root->find("midi.velocity")->bounds();
        f.pointer(ui::PointerEvent::Type::Down, {f.at(0, 60).x, lane.y + 10});
        REQUIRE(f.editor->state().drag == gui::RollState::Drag::Velocity);
    }
    SUBCASE("CC") {
        const ui::Rect lane = f.root->find("midi.cc")->bounds();
        f.pointer(ui::PointerEvent::Type::Down, {f.at(1, 60).x, lane.y + 10});
        REQUIRE_FALSE(f.editor->state().ccDraft.empty());
    }
    f.editor->close();
    REQUIRE(f.editor->open(f.item));
    f.frame();
    CHECK(f.editor->state().drag == gui::RollState::Drag::None);
    CHECK_FALSE(f.editor->state().velocityNote);
    CHECK(f.editor->state().ccDraft.empty());
    const auto version = f.app->head().version;
    f.pointer(ui::PointerEvent::Type::Move, f.at(2, 60));
    f.pointer(ui::PointerEvent::Type::Up, f.at(2, 60));
    CHECK(f.app->head().version == version);
}

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

TEST_CASE("MIDI editor: the CC lane draws a ramp in one step, erases, and switches controllers") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(f.editor->open(f.item));
    f.frame();
    auto& s = f.editor->state();
    s.grid = 0.5;
    auto cc = [&](int number) {
        std::vector<std::pair<double, int>> out;
        const json::Value all = f.exec("midi.notes", R"({"item":")" + f.item + R"("})")["cc"];
        for (const auto& c : all.asArray())
            if (c.find("number")->asInt() == number)
                out.emplace_back(c.find("beat")->asDouble(),
                                 static_cast<int>(c.find("value")->asInt()));
        return out;
    };
    auto steps = [&] { return f.exec("history.list", "{}")["versions"].size(); };
    const ui::Rect lane = f.root->find("midi.cc")->bounds();
    REQUIRE(lane.h > 20);
    const auto xAt = [&](double beat) { return f.at(beat, 60).x + 1; };
    const auto s0 = steps();
    // Draw from beat 0 (low) to beat 2 (high).
    f.pointer(ui::PointerEvent::Type::Down, {xAt(0.0), lane.bottom() - 8});
    f.pointer(ui::PointerEvent::Type::Move, {xAt(1.0), lane.y + lane.h / 2});
    f.pointer(ui::PointerEvent::Type::Move, {xAt(2.0), lane.y + 6});
    f.pointer(ui::PointerEvent::Type::Up, {xAt(2.0), lane.y + 6});
    auto drawn = cc(1);
    REQUIRE(drawn.size() == 3);  // one per grid step the pointer visited
    CHECK(drawn.front().first == doctest::Approx(0.0));
    CHECK(drawn.back().first == doctest::Approx(2.0));
    CHECK(drawn.front().second < 30);
    CHECK(drawn.back().second > 100);
    CHECK(steps() == s0 + 1);
    // Right-drag erases the span.
    ui::PointerEvent down{ui::PointerEvent::Type::Down, {xAt(0.9), lane.y + 10}, 3};
    f.root->pointer(down);
    f.pointer(ui::PointerEvent::Type::Move, {xAt(2.2), lane.y + 10});
    ui::PointerEvent up{ui::PointerEvent::Type::Up, {xAt(2.2), lane.y + 10}, 3};
    f.root->pointer(up);
    f.frame();
    CHECK(cc(1).size() == 1);  // the point at 0 stays
    // The next controller (7, volume) has its own lane.
    REQUIRE(f.root->click("midi.cc.number"));
    CHECK(s.ccNumber == 7);
    CHECK(s.cc.empty());
    f.pointer(ui::PointerEvent::Type::Down, {xAt(1.0), lane.y + 10});
    f.pointer(ui::PointerEvent::Type::Up, {xAt(1.0), lane.y + 10});
    CHECK(cc(7).size() == 1);
    CHECK(cc(1).size() == 1);
    f.snapshot("midi-editor-cc.png");
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
    // Channels: the menu's second row swaps left and right (route.set channels).
    REQUIRE(f.root->click("routing.channels"));
    f.root->frame(false);
    ui::Node* menu = f.root->find("routing.channels.menu");
    REQUIRE(menu);
    const ui::Point swapRow{menu->bounds().x + 30, menu->bounds().y + 26 + 13};
    f.root->pointer({ui::PointerEvent::Type::Move, swapRow});
    f.root->pointer({ui::PointerEvent::Type::Down, swapRow, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, swapRow, 1});
    f.root->frame(false);
    CHECK(m.sendsAt(row(keys), col(drums)).front()->mapTo == std::vector<int>{2, 1});
    CHECK(f.root->find("routing.channels")->valueText() == "Swap R/L");
    // A 4-channel destination offers its aux pair (ADR-0020): row 7, "1/2 -> 3/4".
    f.exec("track.set", R"({"track":")" + drums + R"(","channels":4})");
    matrix.sync();
    REQUIRE(f.root->click("routing.channels"));
    f.root->frame(false);
    menu = f.root->find("routing.channels.menu");
    REQUIRE(menu);
    const ui::Point auxRow{menu->bounds().x + 30, menu->bounds().y + 7 * 26 + 13};
    f.root->pointer({ui::PointerEvent::Type::Move, auxRow});
    f.root->pointer({ui::PointerEvent::Type::Down, auxRow, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, auxRow, 1});
    f.root->frame(false);
    CHECK(m.sendsAt(row(keys), col(drums)).front()->mapTo == std::vector<int>{3, 4});
    CHECK(f.root->find("routing.channels")->valueText() == "1/2\u21923/4");
    REQUIRE(f.root->click("routing.phase"));
    CHECK(m.sendsAt(row(keys), col(drums)).front()->phaseInvert);
    f.snapshot("routing-channels.png");
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
    const auto route =
        f.exec("route.add", R"({"from":")" + strips[0].track + R"(","to":")" + buss + R"("})")["id"]
            .asString();
    mixer.sync();
    auto* send = dynamic_cast<ui::Label*>(f.root->find("mixer.send." + route));
    REQUIRE(send);
    const auto originalSend = send->text();
    auto* originalFader = strips[0].fader;
    f.exec("route.set", R"({"route":")" + route + R"(","volume_db":-12})");
    mixer.sync();
    CHECK(send->text().find("-12.0") != std::string::npos);
    CHECK(strips[0].fader == originalFader);
    f.exec("history.undo", "{}");
    mixer.sync();
    CHECK(send->text() == originalSend);
    f.root->frame(false);
    // The masters stay pinned to the right edge; the others start at the left.
    const ui::Rect row = f.root->find("mixer.strips")->bounds();
    CHECK(static_cast<double>(strips[0].node->bounds().x) ==
          doctest::Approx(static_cast<double>(row.x)));
    CHECK(static_cast<double>(strips[2].node->bounds().right()) ==
          doctest::Approx(static_cast<double>(row.right() - 4)));
    CHECK(strips[2].node->bounds().x > strips[1].node->bounds().right() + 100);
    // "fx" on every strip: the plugin browser for an empty chain.
    auto* fxButton = f.root->find("mixer.fxadd." + strips[0].track);
    REQUIRE(fxButton);
    CHECK(fxButton->valueText() == "fx");
    // The colour stripe has its own row between the name and the FX slots.
    f.exec("track.set", R"({"track":")" + strips[0].track + R"(","color":"teal"})");
    mixer.sync();
    f.root->frame(false);
    auto* stripe = f.root->find("mixer.color." + strips[0].track);
    REQUIRE(stripe);
    CHECK(stripe->bounds().y >= f.root->find("mixer.name." + strips[0].track)->bounds().bottom());
    CHECK(stripe->bounds().bottom() <= f.root->find("mixer.fx." + strips[0].track)->bounds().y);
    // A right-click on the strip opens the track menu.
    const ui::Rect fxNow = f.root->find("mixer.fxadd." + strips[0].track)->bounds();  // rebuilt
    ui::PointerEvent rc;
    rc.type = ui::PointerEvent::Type::Down;
    rc.button = 3;
    rc.pos = {fxNow.x + 4, fxNow.y + 4};
    f.root->pointer(rc);
    rc.type = ui::PointerEvent::Type::Up;
    f.root->pointer(rc);
    f.root->frame(false);
    CHECK(f.root->find("track.menu." + strips[0].track));
    f.root->key(SDLK_ESCAPE);
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

namespace {

void showEventList(Fixture& f) {
    REQUIRE(f.editor->open(f.item));
    f.frame();
    REQUIRE(f.root->click("midi.list.toggle"));
    f.frame();
    REQUIRE(f.root->find("midi.list")->visible);
}

ui::Point eventCell(Fixture& f, std::size_t row, gui::MidiEventList::Column column) {
    const auto* list = f.editor->eventList()->list();
    const auto b = list->bounds();
    return {b.x + (static_cast<float>(column) + 0.5f) * (b.w - ui::ScrollBar::kThickness) / 6,
            b.y + (static_cast<float>(row) + 0.5f) * list->rowHeight - list->scroll()};
}

void editEventCell(Fixture& f, std::size_t row, gui::MidiEventList::Column column,
                   const std::string& text, bool commit = true) {
    const auto p = eventCell(f, row, column);
    f.pointer(ui::PointerEvent::Type::Down, p, 2);
    f.pointer(ui::PointerEvent::Type::Up, p, 2);
    REQUIRE(f.root->focused() == f.root->find("midi.list.edit"));
    f.root->text(text);  // the inline field selects its old value
    f.root->key(commit ? SDLK_RETURN : SDLK_ESCAPE);
    f.frame();
}

std::size_t eventSteps(Fixture& f) { return f.exec("history.list", "{}")["versions"].size(); }

json::Value eventCcs(Fixture& f) {
    return f.exec("midi.notes", R"({"item":")" + f.item + R"("})")["cc"];
}

}  // namespace

TEST_CASE("MIDI event list: toggle, ordered rows, formatting and shared selection") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    f.exec("midi.edit", R"({"item":")" + f.item + R"(","ops":[
      {"op":"cc.set","number":64,"channel":2,"points":[{"beat":0,"value":127}]},
      {"op":"cc.set","number":1,"points":[{"beat":0.5,"value":32}]}]})");
    showEventList(f);
    auto* events = f.editor->eventList();
    REQUIRE(events->list()->count() == 6);
    using Text = gui::MidiEventList::RowText;
    CHECK(events->rowText(0) == Text{"2.1.0", "Note", "1.0", "C4", "96", "1"});
    CHECK(events->rowText(1) == Text{"2.1.0", "CC 64", "", "64", "127", "2"});
    CHECK(events->rowText(2) == Text{"2.1.480", "CC 1", "", "1", "32", "1"});
    CHECK(events->rowText(3)[0] == "2.2.0");
    CHECK(events->rowText(3)[3] == "E4");
    CHECK_FALSE(f.root->find("midi.left")->visible);
    CHECK(f.root->find("midi.notation")->visible);
    f.pointer(ui::PointerEvent::Type::Down, eventCell(f, 3, gui::MidiEventList::Column::Pitch));
    f.pointer(ui::PointerEvent::Type::Up, eventCell(f, 3, gui::MidiEventList::Column::Pitch));
    CHECK(f.editor->state().selected == std::set<std::size_t>{1});
    REQUIRE(f.root->click("midi.list.toggle"));
    f.frame();
    CHECK_FALSE(f.root->find("midi.list")->visible);
    CHECK(f.root->find("midi.left")->visible);
    CHECK(f.editor->roll()->visible);
    CHECK(f.root->find("midi.notation")->visible);
    CHECK(f.editor->state().selected == std::set<std::size_t>{1});
    f.editor->state().selected = {2, 3};
    REQUIRE(f.root->click("midi.list.toggle"));
    f.frame();
    CHECK(events->list()->selected() == 4);
    f.snapshot("midi-event-list.png");
}

TEST_CASE(
    "MIDI event list: text edits each commit one version, invalid input and Escape commit none") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    showEventList(f);
    using Col = gui::MidiEventList::Column;
    const auto initial = eventSteps(f);
    editEventCell(f, 0, Col::Value, "110");
    CHECK(f.notes().asArray()[0]["velocity"].asInt() == 110);
    CHECK(eventSteps(f) == initial + 1);
    editEventCell(f, 0, Col::Position, "2.1.480");
    CHECK(f.notes().asArray()[0]["start"].asDouble() == doctest::Approx(0.5));
    CHECK(eventSteps(f) == initial + 2);
    editEventCell(f, 0, Col::Pitch, "C#4");
    CHECK(f.notes().asArray()[0]["pitch"].asInt() == 61);
    CHECK(eventSteps(f) == initial + 3);
    editEventCell(f, 0, Col::Pitch, "62");
    CHECK(f.notes().asArray()[0]["pitch"].asInt() == 62);
    CHECK(eventSteps(f) == initial + 4);
    editEventCell(f, 0, Col::Length, "1.240");
    CHECK(f.notes().asArray()[0]["length"].asDouble() == doctest::Approx(1.25));
    CHECK(eventSteps(f) == initial + 5);
    for (const auto& bad : {std::pair{Col::Value, "128"},
                            {Col::Value, "-1"},
                            {Col::Position, "2.5.0"},
                            {Col::Position, "2.1.960"},
                            {Col::Position, "garbage"},
                            {Col::Length, "0.0"},
                            {Col::Pitch, "C#"},
                            {Col::Pitch, "9999999999999999"}}) {
        editEventCell(f, 0, bad.first, bad.second);
        CHECK(eventSteps(f) == initial + 5);
        CHECK_FALSE(f.root->find("midi.list.status")->valueText().empty());
        f.root->key(SDLK_ESCAPE);
        f.frame();
    }
    editEventCell(f, 0, Col::Value, "64", false);
    CHECK(f.notes().asArray()[0]["velocity"].asInt() == 110);
    CHECK(eventSteps(f) == initial + 5);
    CHECK_FALSE(f.root->find("midi.list.edit")->visible);
    f.exec("history.undo", "{}");
    f.frame();
    CHECK(f.editor->eventList()->rowText(0)[2] == "1.0");
}

TEST_CASE("MIDI event list: multi-row Delete and quarter-note addition") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    f.exec("midi.edit", R"({"item":")" + f.item + R"(","ops":[
      {"op":"cc.set","number":1,"channel":2,"points":[{"beat":0.5,"value":32},{"beat":0.5010416667,"value":48}]},
      {"op":"cc.set","number":1,"points":[{"beat":0.5,"value":64}]}]})");
    showEventList(f);
    const auto initial = eventSteps(f);
    using Col = gui::MidiEventList::Column;
    auto p = eventCell(f, 0, Col::Position);
    f.pointer(ui::PointerEvent::Type::Down, p);
    f.pointer(ui::PointerEvent::Type::Up, p);
    p = eventCell(f, 2, Col::Position);  // channel 2 CC
    f.pointer(ui::PointerEvent::Type::Down, p, 1, ui::kCtrl);
    f.pointer(ui::PointerEvent::Type::Up, p, 1, ui::kCtrl);
    f.root->key(SDLK_DELETE);
    f.frame();
    CHECK(f.notes().size() == 3);
    REQUIRE(eventCcs(f).size() == 2);
    CHECK(eventCcs(f).asArray()[0]["value"].asInt() == 64);
    CHECK(eventCcs(f).asArray()[1]["value"].asInt() == 48);  // adjacent tick survived
    CHECK(eventSteps(f) == initial + 1);
    REQUIRE(f.root->click("midi.list.add"));
    f.frame();
    CHECK(f.notes().size() == 4);
    CHECK(f.notes().asArray()[0]["start"].asDouble() == 0);
    CHECK(f.notes().asArray()[0]["pitch"].asInt() == 60);
    CHECK(f.notes().asArray()[0]["velocity"].asInt() == 96);
    CHECK(f.notes().asArray()[0]["length"].asDouble() == 1);
    CHECK(f.editor->state().selected == std::set<std::size_t>{0});
    CHECK(eventSteps(f) == initial + 2);
    // Add at a selected row, then delete all rows with a Shift selection.
    p = eventCell(f, 4, Col::Position);
    f.pointer(ui::PointerEvent::Type::Down, p);
    f.pointer(ui::PointerEvent::Type::Up, p);
    REQUIRE(f.root->click("midi.list.add"));
    f.frame();
    CHECK(f.notes().size() == 5);
    CHECK(f.notes().asArray()[2]["pitch"].asInt() == 60);
    CHECK(f.notes().asArray()[2]["start"].asDouble() == 2);
    CHECK(eventSteps(f) == initial + 3);
    f.root->key(SDLK_HOME);
    f.root->key(SDLK_END, ui::kShift);
    f.root->key(SDLK_DELETE);
    f.frame();
    CHECK(f.notes().size() == 0);
    CHECK(eventCcs(f).size() == 0);
    CHECK(eventSteps(f) == initial + 4);
}

TEST_CASE("MIDI event list: CC text edits preserve controller channels and neighbors") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    f.exec("midi.edit", R"({"item":")" + f.item + R"(","ops":[
      {"op":"cc.set","number":11,"channel":3,"points":[{"beat":0.5,"value":32},{"beat":0.5010416667,"value":48}]}]})");
    showEventList(f);
    using Col = gui::MidiEventList::Column;
    const auto initial = eventSteps(f);
    editEventCell(f, 1, Col::Value, "0");
    CHECK(eventCcs(f).asArray()[0]["value"].asInt() == 0);
    CHECK(eventCcs(f).asArray()[0]["channel"].asInt() == 3);
    CHECK(eventCcs(f).asArray()[1]["value"].asInt() == 48);
    CHECK(eventSteps(f) == initial + 1);
    editEventCell(f, 1, Col::Position, "2.2.480");
    CHECK(eventCcs(f).asArray()[1]["beat"].asDouble() == doctest::Approx(1.5));
    CHECK(eventCcs(f).asArray()[0]["value"].asInt() == 48);
    CHECK(eventSteps(f) == initial + 2);
    editEventCell(f, 3, Col::Pitch, "7");
    CHECK(eventCcs(f).asArray()[1]["number"].asInt() == 7);
    CHECK(eventCcs(f).asArray()[1]["channel"].asInt() == 3);
    CHECK(eventSteps(f) == initial + 3);
}

TEST_CASE("MIDI event list: project meter, source offset and playrate, virtualization") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    f.exec("timesig.set", R"({"bar":2,"meter":"3/8"})");
    auto take = f.app->head().items.find(*model::Id::parse(f.item))->activeTake;
    REQUIRE(take);
    f.exec("take.set", R"({"take":")" + take->str() + R"(","start_offset":0.5,"playrate":2})");
    showEventList(f);
    CHECK(f.editor->eventList()->rowText(1)[0] == "2.1.480");
    CHECK(f.editor->eventList()->rowText(1)[2] == "0.480");
    editEventCell(f, 1, gui::MidiEventList::Column::Position, "3.1.240");
    CHECK(f.notes().asArray()[3]["start"].asDouble() == doctest::Approx(1.625));
    CHECK(f.editor->state().selected == std::set<std::size_t>{3});
    auto ops = json::Value::array();
    for (int i = 0; i < 2000; ++i) {
        auto op = json::Value::object();
        op["op"] = "add";
        op["pitch"] = i % 128;
        op["start"] = 2.0 + i / 8.0;
        op["length"] = 0.25;
        ops.push_back(std::move(op));
    }
    REQUIRE(f.editor->edit(std::move(ops)));
    f.frame();
    const auto* list = f.editor->eventList()->list();
    CHECK(list->count() == 2004);
    CHECK(list->rowsPainted() < 20);
    f.root->key(SDLK_END);
    f.frame();
    CHECK(list->selected() == 2003);
    CHECK(list->scroll() > 1000);
    CHECK(list->rowsPainted() < 20);
}

TEST_CASE("MIDI event list: stale inline edits and closing cannot target hidden rows") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    showEventList(f);
    const auto p = eventCell(f, 0, gui::MidiEventList::Column::Value);
    f.pointer(ui::PointerEvent::Type::Down, p, 2);
    f.pointer(ui::PointerEvent::Type::Up, p, 2);
    f.root->text("120");
    const auto initial = eventSteps(f);
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"move","notes":[0],"beats":3.5}]})");
    // Another client edited before the next UI sync: refuse the stale index.
    f.root->key(SDLK_RETURN);
    CHECK(eventSteps(f) == initial + 1);
    CHECK(f.notes().asArray()[3]["velocity"].asInt() == 96);
    CHECK_FALSE(f.root->find("midi.list.edit")->visible);
    CHECK_FALSE(f.root->find("midi.list.status")->valueText().empty());
    f.frame();
    f.root->focus(f.editor->eventList()->list());
    f.root->key(SDLK_HOME);
    f.editor->close();
    CHECK(f.root->focused() == nullptr);
    CHECK(f.editor->eventList()->list()->count() == 0);
    f.root->key(SDLK_DELETE);
    CHECK(eventSteps(f) == initial + 1);
    REQUIRE(f.editor->open(f.item));
    f.frame();
    CHECK(f.editor->eventList()->list()->count() == 4);
    CHECK(f.editor->state().selected.empty());
}

TEST_CASE("MIDI external edits clear identity and reject a gesture before the next sync") {
    Fixture f;
    REQUIRE(f.ok);
    REQUIRE(f.editor->open(f.item));
    f.frame();
    f.editor->state().selected = {0};
    const auto version = f.editor->beginGesture();
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"add","pitch":58,"start":0,"length":1}]})");
    const auto external = f.app->head().version;
    json::Value ops = json::parse(R"([{"op":"remove","notes":[0]}])").value();
    auto result = f.editor->edit(std::move(ops), version);
    CHECK_FALSE(result);
    CHECK(f.app->head().version == external);
    CHECK(f.editor->state().selected.empty());
    CHECK(f.notes().size() == 5);
    f.editor->state().selected = {1};
    f.exec("midi.edit",
           R"({"item":")" + f.item + R"(","ops":[{"op":"velocity","notes":[1],"velocity":50}]})");
    f.frame();
    CHECK(f.editor->state().selected.empty());
}

TEST_CASE("MIDI notation follows project meters at the item's placement") {
    Fixture f;
    REQUIRE(f.ok);
    f.exec("timesig.set", R"({"bar":2,"meter":"3/4"})");
    f.exec("timesig.set", R"({"bar":3,"meter":"5/8"})");
    REQUIRE(f.editor->open(f.item));
    f.frame();
    CHECK(f.editor->notation()->text().find("meter 4/4") == std::string::npos);
    // Header disagreement is caught by live compile before midi.write.
    auto* text = f.editor->notation();
    f.root->focus(text);
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("meter 4/4; voice v; 1: C4:w");
    const auto version = f.app->head().version;
    text->onSubmit(text->text());
    CHECK(f.app->head().version == version);
    CHECK_FALSE(text->marks().empty());
    f.root->key(SDLK_A, ui::kCtrl);
    f.root->text("meter 3/4; meter 5/8 at 2; voice v; 1: C4:h. | 2: D4:q E4:q F4:e");
    text->onSubmit(text->text());
    CHECK(text->marks().empty());
    CHECK(f.notes().size() == 4);
}

TEST_CASE("routing matrix clamps scrolling when channel rows are deleted") {
    Fixture f;
    REQUIRE(f.ok);
    std::vector<std::string> tracks;
    for (int i = 0; i < 30; ++i)
        tracks.push_back(
            f.exec("track.add", R"({"type":"buss","name":"Bus"})")["track"].asString());
    gui::RoutingMatrix matrix(*f.root, *f.actions, *f.dispatcher, *f.app);
    matrix.open();
    f.root->frame(false);
    const auto bounds = matrix.grid()->bounds();
    ui::PointerEvent wheel;
    wheel.type = ui::PointerEvent::Type::Wheel;
    wheel.pos = {bounds.x + 201, bounds.y + 123};
    wheel.wheelY = -100;
    wheel.wheelX = -100;
    matrix.grid()->onPointer(wheel);
    for (const auto& track : tracks) f.exec("track.remove", R"({"track":")" + track + R"("})");
    matrix.sync();
    f.root->frame(false);
    // Row zero and column zero return to their ordinary screen positions.
    ui::PointerEvent hover;
    hover.type = ui::PointerEvent::Type::Move;
    hover.pos = {bounds.x + 201, bounds.y + 123};
    matrix.grid()->onPointer(hover);
    auto* status = f.root->find("routing.status");
    REQUIRE(status);
    CHECK(status->valueText().find(matrix.model().blocked(0, 0)) != std::string::npos);
    CHECK(status->valueText().find("not allowed") != std::string::npos);
}
