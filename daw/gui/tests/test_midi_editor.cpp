// The MIDI editor driven like a user: pointer and keys through the root, the
// notation panel typed into, the session read back through commands.
#include <doctest/doctest.h>
#include <unistd.h>

#include <SDL3/SDL_keycode.h>

#include <filesystem>
#include <thread>

#include "app_actions.h"
#include "midi_editor.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
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
