// The mixer's own gestures (gui.md §4.4): the fx button, a resizable chain
// section, plug-in reorder and copy by drag, strip reorder by drag; the
// reorder-index helper the TCP shares.
#include <SDL3/SDL_keycode.h>
#include <doctest/doctest.h>
#include <unistd.h>
#include "product_fixture.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "app_actions.h"
#include "meter_feed.h"
#include "mixer.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/ui/controls.h"
#include "supple/ui/root.h"
#include "supple/ui/widgets.h"

using namespace supple;

namespace {

const cmd::Origin kTest{cmd::Origin::Type::Test, "mixer-test", {}};

bool near(float a, float b) { return std::fabs(a - b) < 0.6f; }

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
    std::unique_ptr<gui::MeterFeed> feed;
    std::unique_ptr<gui::Mixer> mixer;
    std::vector<std::string> editors, errors;  // ui.fx.editor calls; reported errors
    bool ok = false;

    Fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-mixer-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "config");
        ::setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"mixer-test", 1400, 900, true});
        if (!w) return;
        window = std::move(*w);
        auto f = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        if (!f) return;
        fonts = *f;
        engine::AppOptions options;
        options.workdir = dir;
        options.pluginPath = {std::filesystem::path(SUPPLE_CLAP_DIR)};
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        root = std::make_unique<ui::Root>(*window, fonts, ui::Theme::dark());
        actions = std::make_unique<gui::AppActions>(
            registry, *dispatcher, *app, ui, actions::Keymap::defaultFile(gui::test::slug()));
        actions->addUi("ui.fx.editor", "FX: open or close editor", "fx",
                       [this](const json::Value& v) -> Status {
                           editors.push_back(v.asString());
                           return {};
                       });
        actions->onError = [this](const std::string& e) { errors.push_back(e); };
        feed = std::make_unique<gui::MeterFeed>([] { return std::vector<gui::StripReading>{}; });
        mixer = std::make_unique<gui::Mixer>(*root, *actions, *dispatcher, *app, *feed);
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->emplace<ui::Node>("arrange").layout.height = ui::SizeRule::fraction();
        frame->add(mixer->build());
        root->setContent(std::move(frame));
        mixer->setVisible(true);
        root->frame(false);
        ok = true;
    }
    ~Fixture() {
        mixer.reset();
        actions.reset();
        root.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }

    json::Value exec(const std::string& command, const std::string& args) {
        auto r = dispatcher->execute({command, json::parse(args).value(), kTest});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    }
    std::string addTrack(const char* type, const char* name) {
        return exec("track.add",
                    std::string(R"({"type":")") + type + R"(","name":")" + name + R"("})")["track"]
            .asString();
    }
    std::string addFx(const std::string& track, const char* plugin) {
        return exec("fx.add", R"({"track":")" + track + R"(","plugin":")" + plugin + R"("})")["fx"]
            .asString();
    }
    void refresh() {
        mixer->sync();
        root->frame(false);
    }
    const model::Track& track(const std::string& id) {
        return *app->head().tracks.find(*model::Id::parse(id));
    }
    std::vector<std::string> chain(const std::string& id) {
        std::vector<std::string> out;
        for (model::Id fx : track(id).chain) out.push_back(fx.str());
        return out;
    }
    std::vector<std::string> children(const std::string& id) {
        std::vector<std::string> out;
        for (model::Id c : track(id).children) out.push_back(c.str());
        return out;
    }
    std::size_t steps() { return exec("history.list", "{}")["versions"].size(); }
    const gui::Mixer::Strip& strip(const std::string& id) {
        for (const auto& s : mixer->strips())
            if (s.track == id) return s;
        FAIL("no strip " << id);
        return mixer->strips().front();
    }
    ui::Node* node(const std::string& id) {
        ui::Node* n = root->find(id);
        REQUIRE_MESSAGE(n, id);
        return n;
    }
    ui::Point centre(const ui::Node* n) {
        const ui::Rect b = n->bounds();
        return {b.x + b.w / 2, b.y + b.h / 2};
    }
    void pointer(ui::PointerEvent::Type type, ui::Point p, int clicks = 1) {
        ui::PointerEvent e;
        e.type = type;
        e.pos = p;
        e.button = type == ui::PointerEvent::Type::Move ? 0 : 1;
        e.clicks = clicks;
        root->pointer(e);
        root->frame(false);
    }
    // Down, a few moves, up: a drag the way SDL would deliver it.
    void drag(ui::Point from, ui::Point to, bool release = true) {
        pointer(ui::PointerEvent::Type::Down, from);
        for (int i = 1; i <= 4; ++i)
            pointer(ui::PointerEvent::Type::Move,
                    {from.x + (to.x - from.x) * static_cast<float>(i) / 4,
                     from.y + (to.y - from.y) * static_cast<float>(i) / 4});
        if (release) pointer(ui::PointerEvent::Type::Up, to);
    }
    // A point on a strip no control uses: its sends rows.
    ui::Point empty(const std::string& track) { return centre(node("mixer.sends." + track)); }
    void snapshot(const char* name) {
        const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS");
        if (!out) return;
        root->damageAll();
        root->frame(false);
        (void)gui::writePng(std::filesystem::path(out) / name, root->readBack());
    }
};

}  // namespace

TEST_CASE("mixer: a master looks like the others, and \"fx\" sits small at the strip's top right") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto master = std::string("tr_0");
    f.refresh();
    const auto& sa = f.strip(a);
    const auto& sm = f.strip(master);
    // The same colour just inside the left edge below the stripe, on both.
    auto pixel = [&](const ui::Rect& r) {
        const gui::Image image = f.root->readBack();
        const int x = static_cast<int>(r.x) + 1, y = static_cast<int>(r.y) + 120;
        const std::size_t at =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
             static_cast<std::size_t>(x)) *
            4;
        return std::vector<std::uint8_t>(image.rgba.begin() + static_cast<std::ptrdiff_t>(at),
                                         image.rgba.begin() + static_cast<std::ptrdiff_t>(at) + 4);
    };
    CHECK(pixel(sm.node->bounds()) == pixel(sa.node->bounds()));
    // Small, at the top right, with the name beside it.
    const ui::Rect strip = sa.node->bounds(), fx = sa.fx->bounds();
    CHECK(sa.fx->valueText() == "fx");
    CHECK(fx.w <= 34);
    CHECK(fx.h <= 16);
    CHECK(fx.right() >= strip.right() - 8);
    CHECK(fx.y < strip.y + 12);
    CHECK(sa.name->bounds().right() <= fx.x + 1);
    // Empty chain: the plug-in browser; filled: the chain, with the count.
    f.addFx(a, "Plumb");
    f.refresh();
    CHECK(f.strip(a).fx->valueText() == "fx 1");
}

TEST_CASE(
    "mixer: the chain section lists the whole chain, scrolled when it is longer than it fits") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    for (int i = 0; i < 12; ++i) f.addFx(a, "Plumb");
    f.refresh();
    ui::Node* list = f.strip(a).chain;
    // The default height fits five plug-ins whole.
    int whole = 0;
    for (const auto& c : list->children())
        if (c->id().rfind("mixer.fx.fx_", 0) == 0 &&
            c->bounds().bottom() <= list->bounds().bottom() + 0.5f)
            ++whole;
    CHECK(whole >= 5);
    CHECK(list->children().size() >= 13);  // twelve slots and the bar
    // The wheel scrolls the chain, not the strips.
    const float y0 = f.node("mixer.fx.fx_1")->bounds().y;
    ui::PointerEvent wheel;
    wheel.type = ui::PointerEvent::Type::Wheel;
    wheel.pos = f.centre(f.node("mixer.fx.fx_2"));
    wheel.wheelY = -2;
    f.root->pointer(wheel);
    f.root->frame(false);
    CHECK(f.node("mixer.fx.fx_1")->bounds().y < y0 - 10);
    // Slots that are scrolled out are not hit.
    CHECK(f.root->content()->hitTest({list->bounds().x + 5, list->bounds().y - 6}) !=
          f.node("mixer.fx.fx_1"));
}

TEST_CASE("mixer: dragging the top edge resizes only the chain section; double-click resets") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    f.refresh();
    ui::Node* panel = f.node("mixer");
    const float standard = panel->bounds().h;
    CHECK(near(standard, gui::Mixer::defaultHeight()));
    // About five plug-ins: the chain section is five rows tall.
    CHECK(near(f.strip(a).chain->bounds().h, 5 * 18 + 4 * 2));
    const ui::Rect fader = f.strip(a).fader->bounds();
    const float meter = f.strip(a).meter->bounds().h;
    const float chain = f.strip(a).chain->bounds().h;
    const float edge = f.node("mixer.grip")->bounds().y + 2;
    f.drag({700, edge}, {700, edge - 120});
    CHECK(near(panel->bounds().h, standard + 120));
    CHECK(near(f.mixer->height(), standard + 120));
    CHECK(near(f.strip(a).chain->bounds().h, chain + 120));
    CHECK(near(f.strip(a).fader->bounds().h, fader.h));  // the rest stays
    CHECK(near(f.strip(a).meter->bounds().h, meter));
    // Down is shorter, to a floor of two plug-ins; up stops short of the arrange view.
    f.drag({700, f.node("mixer.grip")->bounds().y + 2}, {700, 899});
    CHECK(near(panel->bounds().h, gui::Mixer::defaultHeight() - 3 * 20));
    const float top = f.node("mixer.grip")->bounds().y + 2;
    f.drag({700, top}, {700, -500});
    CHECK(panel->bounds().h <= 900 - 120);
    CHECK(panel->bounds().h > standard + 120);
    // Double-click on the edge: the default again.
    const ui::Point grip = f.centre(f.node("mixer.grip"));
    f.pointer(ui::PointerEvent::Type::Down, grip, 1);
    f.pointer(ui::PointerEvent::Type::Up, grip);
    f.pointer(ui::PointerEvent::Type::Down, grip, 2);
    f.pointer(ui::PointerEvent::Type::Up, grip);
    CHECK(near(panel->bounds().h, standard));
    CHECK(near(f.mixer->height(), standard));
    // The height is state the app saves and restores, within the window.
    f.mixer->setHeight(500);
    f.refresh();
    CHECK(near(panel->bounds().h, 500));
    f.mixer->setHeight(5000);
    f.refresh();
    CHECK(panel->bounds().h <= 900 - 120);
    f.mixer->setHeight(470);
    const json::Value saved = f.mixer->saveLayout();
    CHECK(near(static_cast<float>(saved.find("height")->asDouble()), 470));
    f.mixer->setHeight(500);
    f.mixer->loadLayout(saved);
    CHECK(near(f.mixer->height(), 470));
    f.mixer->setHeight(10);
    CHECK(f.mixer->height() >= gui::Mixer::defaultHeight() - 4 * 20);
}

TEST_CASE("mixer: a click on a plug-in still opens it; a drag within the chain reorders it") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto p1 = f.addFx(a, "Plumb"), p2 = f.addFx(a, "Plumb"), p3 = f.addFx(a, "Plumb");
    f.refresh();
    const auto before = f.steps();
    // A click, or a nudge under the threshold, is not a drag.
    REQUIRE(f.root->click("mixer.fx." + p2));
    CHECK(f.editors == std::vector<std::string>{p2});
    const ui::Point c = f.centre(f.node("mixer.fx." + p1));
    f.pointer(ui::PointerEvent::Type::Down, c);
    f.pointer(ui::PointerEvent::Type::Move, {c.x + 2, c.y + 1});
    f.pointer(ui::PointerEvent::Type::Up, {c.x + 2, c.y + 1});
    CHECK(f.editors.size() == 2);
    CHECK(f.steps() == before);
    // The first plug-in dragged below the last: the chain's end, one step.
    const ui::Point from = f.centre(f.node("mixer.fx." + p1));
    const ui::Rect last = f.node("mixer.fx." + p3)->bounds();
    f.drag(from, {last.x + 20, last.bottom() + 1}, false);
    CHECK(f.mixer->drag().active);
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::Reorder);
    f.snapshot("mixer-drag-fx.png");
    f.pointer(ui::PointerEvent::Type::Up, {last.x + 20, last.bottom() + 1});
    CHECK_FALSE(f.mixer->drag().active);
    CHECK(f.editors.size() == 2);  // the release was not a click
    CHECK(f.chain(a) == std::vector<std::string>{p2, p3, p1});
    CHECK(f.steps() == before + 1);
    f.refresh();
    // Back to the front.
    const ui::Point again = f.centre(f.node("mixer.fx." + p1));
    f.drag(again, {again.x, f.node("mixer.fx." + p2)->bounds().y - 1});
    CHECK(f.chain(a) == std::vector<std::string>{p1, p2, p3});
    // Dropped where it already is: nothing.
    f.refresh();
    const auto steps = f.steps();
    const ui::Point same = f.centre(f.node("mixer.fx." + p2));
    f.drag(same, {same.x + 20, same.y});
    CHECK(f.steps() == steps);
    // Escape cancels the drag, and the release after it is no click.
    const ui::Point esc = f.centre(f.node("mixer.fx." + p1));
    f.drag(esc, {esc.x, esc.y + 50}, false);
    CHECK(f.mixer->drag().active);
    f.root->key(SDLK_ESCAPE);
    CHECK_FALSE(f.mixer->drag().active);
    f.pointer(ui::PointerEvent::Type::Up, {esc.x, esc.y + 50});
    CHECK(f.chain(a) == std::vector<std::string>{p1, p2, p3});
    CHECK(f.steps() == steps);
    CHECK(f.editors.size() == 2);
    // Dropped somewhere that is nobody's strip: cancelled.
    f.drag(f.centre(f.node("mixer.fx." + p1)), {700, 20});
    CHECK(f.chain(a) == std::vector<std::string>{p1, p2, p3});
    CHECK(f.steps() == steps);
    CHECK(f.errors.empty());
}

TEST_CASE(
    "mixer: dragging a plug-in onto another strip copies it, settings and all, last in its chain") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A");
    const auto b = f.addTrack("channel", "B");
    const auto master = std::string("tr_0");
    const auto p1 = f.addFx(a, "Plumb");
    const auto b1 = f.addFx(b, "Plumb");
    f.exec("fx.set", R"({"fx":")" + p1 + R"(","label":"Mine","wet":40})");
    f.refresh();
    const auto before = f.steps();
    // Onto any part of the other strip: here its meter column.
    f.drag(f.centre(f.node("mixer.fx." + p1)), f.centre(f.node("mixer.faderrow." + b)), false);
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::Copy);
    CHECK(f.mixer->drag().target == b);
    f.snapshot("mixer-drag-copy.png");
    f.pointer(ui::PointerEvent::Type::Up, f.centre(f.node("mixer.faderrow." + b)));
    REQUIRE(f.chain(b).size() == 2);
    CHECK(f.chain(b)[0] == b1);
    CHECK(f.chain(a) == std::vector<std::string>{p1});  // the original stays
    const auto& copy = *f.app->head().fx.find(*model::Id::parse(f.chain(b)[1]));
    CHECK(copy.label == "Mine");
    CHECK(copy.wet == 40);
    CHECK(f.steps() == before + 1);
    f.refresh();
    CHECK(f.strip(b).fx->valueText() == "fx 2");
    // Onto a master too; and one undo takes the copy away.
    f.drag(f.centre(f.node("mixer.fx." + p1)), f.empty(master));
    CHECK(f.chain(master).size() == 1);
    f.exec("history.undo", "{}");
    CHECK(f.chain(master).empty());
    CHECK(f.errors.empty());
    f.refresh();
    f.snapshot("mixer-chain.png");
}

TEST_CASE("mixer: dragging a strip's empty part reorders it among its own kind, in the model") {
    Fixture f;
    if (!f.ok) return;
    const auto a = f.addTrack("channel", "A"), b = f.addTrack("channel", "B"),
               c = f.addTrack("channel", "C");
    const auto bus = f.addTrack("buss", "Bus");
    const auto master = std::string("tr_0");
    f.refresh();
    // Channels, then the buss, then the master.
    REQUIRE(f.mixer->strips().size() == 5);
    CHECK(f.children(master) == std::vector<std::string>{a, b, c, bus});
    const auto before = f.steps();
    // A to after C: the insertion bar is shown while it moves.
    const ui::Rect rc = f.strip(c).node->bounds();
    f.drag(f.empty(a), {rc.x + rc.w - 10, rc.y + 200}, false);
    CHECK(f.mixer->drag().kind == gui::Mixer::DragView::Kind::Strip);
    f.snapshot("mixer-drag-strip.png");
    CHECK(f.mixer->drag().drop == gui::Mixer::DragView::Drop::Reorder);
    f.pointer(ui::PointerEvent::Type::Up, {rc.x + rc.w - 10, rc.y + 200});
    CHECK(f.children(master) == std::vector<std::string>{b, c, a, bus});  // not after the buss
    CHECK(f.steps() == before + 1);
    f.refresh();
    CHECK(f.mixer->strips()[0].track == b);
    CHECK(f.mixer->strips()[2].track == a);
    // C to the front.
    f.drag(f.empty(c), {f.strip(b).node->bounds().x + 5, f.empty(b).y});
    CHECK(f.children(master) == std::vector<std::string>{c, b, a, bus});
    f.refresh();
    // Over the buss strip or the master: no drop. Escape: none either.
    const auto steps = f.steps();
    f.drag(f.empty(c), f.empty(bus));
    f.drag(f.empty(c), f.empty(master));
    CHECK(f.children(master) == std::vector<std::string>{c, b, a, bus});
    f.drag(f.empty(c), f.empty(a), false);
    f.root->key(SDLK_ESCAPE);
    f.pointer(ui::PointerEvent::Type::Up, f.empty(a));
    CHECK(f.children(master) == std::vector<std::string>{c, b, a, bus});
    // A drag of the empty bottom (nobody's strip): cancelled.
    f.drag(f.empty(c), {700, 20});
    CHECK(f.steps() == steps);
    // The master (tr_0) stays put; a button press is not a drag.
    f.drag(f.empty(master), f.empty(c));
    CHECK(f.steps() == steps);
    f.drag(f.centre(f.node("mixer.mute." + c)), f.empty(a));
    CHECK(f.steps() == steps);  // a control is not a handle: nothing moved
    CHECK(f.children(master) == std::vector<std::string>{c, b, a, bus});
    // Undo restores the order, a step at a time.
    f.exec("history.undo", "{}");
    CHECK(f.children(master) == std::vector<std::string>{b, c, a, bus});
    f.exec("history.undo", "{}");
    CHECK(f.children(master) == std::vector<std::string>{a, b, c, bus});
    CHECK(f.errors.empty());
}

TEST_CASE("mixer: masters reorder among masters; tr_0 stays first") {
    Fixture f;
    if (!f.ok) return;
    const auto m2 = f.addTrack("master", "M2"), m3 = f.addTrack("master", "M3");
    f.refresh();
    const auto order = [&] {
        std::vector<std::string> out;
        for (model::Id m : f.app->head().project->masters) out.push_back(m.str());
        return out;
    };
    REQUIRE(order() == std::vector<std::string>{"tr_0", m2, m3});
    // M3 to the left of M2 (tr_0 still first).
    f.drag(f.empty(m3), {f.strip(m2).node->bounds().x + 5, f.empty(m2).y});
    CHECK(order() == std::vector<std::string>{"tr_0", m3, m2});
    f.refresh();
    // Left of tr_0 is not allowed: it stays first.
    f.drag(f.empty(m2), {f.strip("tr_0").node->bounds().x + 5, f.empty("tr_0").y});
    CHECK(order()[0] == "tr_0");
    CHECK(f.errors.empty());
}
