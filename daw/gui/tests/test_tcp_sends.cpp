// Output gestures through the shell, Root pointer capture and the real bus.
#include <SDL3/SDL_keycode.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>

#include "routing_matrix.h"
#include "shell.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"

using namespace supple;

namespace {
const cmd::Origin kSendUi{cmd::Origin::Type::Ui, "gui", {}};
using Pointer = ui::PointerEvent::Type;

struct SendsFixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    gui::UiState state;
    gui::MeterFeed meters;
    std::unique_ptr<gui::AppActions> actions;
    std::unique_ptr<peaks::PeakService> peaks;
    std::unique_ptr<ui::Root> root;
    std::unique_ptr<gui::Shell> shell;
    model::Id a{}, b{};
    std::vector<std::string> errors;
    std::vector<std::pair<model::Id, ui::Rect>> opened;

    explicit SendsFixture(bool busses = false) {
        dir = std::filesystem::path(SUPPLE_SKILL_DIR).parent_path() / "build" / "p5-sends" /
              ("fixture-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        REQUIRE(p);
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"send-test", 1100, 600, true});
        REQUIRE(w);
        window = std::move(*w);
        auto fonts = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        REQUIRE(fonts);
        engine::AppOptions options;
        options.workdir = dir;
        auto engineApp = engine::EngineApp::create(options);
        REQUIRE(engineApp);
        app = std::move(*engineApp);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
        a = add(busses ? "buss" : "channel", "A");
        b = add("buss", "B");
        actions = std::make_unique<gui::AppActions>(registry, *dispatcher, *app, state,
                                                    dir / "keys.json");
        peaks = std::make_unique<peaks::PeakService>(dir / "cache", [](std::function<void()>) {});
        root = std::make_unique<ui::Root>(*window, *fonts, ui::Theme::dark());
        shell = std::make_unique<gui::Shell>(*app, *dispatcher, state, "", *peaks, meters);
        shell->onError = [this](const std::string& error) { errors.push_back(error); };
        shell->onRouting = [this](model::Id track, const ui::Rect& anchor) {
            opened.emplace_back(track, anchor);
        };
        root->setContent(shell->build(*root, *actions));
        shell->installOverlay(*root);
        root->onUnhandledKey = [this](const ui::KeyEvent& e) {
            return actions->key(e.key, e.mods, false);
        };
        sync();
    }
    ~SendsFixture() {
        root.reset();  // meters detach while the shell and feed still exist
        shell.reset();
        peaks.reset();
        actions.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    json::Value exec(const std::string& command, json::Value args = json::Value::object()) {
        auto r = dispatcher->execute({command, std::move(args), kSendUi});
        INFO((r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    }
    model::Id add(const std::string& type, const std::string& name) {
        json::Value args = json::Value::object();
        args["type"] = type;
        args["name"] = name;
        const auto result = exec("track.add", std::move(args));
        const auto id = model::Id::parse(result.find("track")->asString());
        REQUIRE(id);
        return *id;
    }
    void sync() {
        shell->sync();
        root->frame(false);
    }
    views::ArrangeView& view() { return *shell->arrange(); }
    ui::Node& out(model::Id track) {
        auto* node = view().tcpPanel().controlNode(track, "out");
        REQUIRE(node);
        return *node;
    }
    static ui::Point centre(const ui::Rect& r) { return {r.x + r.w / 2, r.y + r.h / 2}; }
    ui::Point target(model::Id track, bool lanes = false) {
        const auto row = view().trackLayout().indexOf(track);
        REQUIRE(row);
        return centre(lanes ? view().rowRect(*row) : view().tcpRowRect(*row));
    }
    std::size_t history() { return exec("history.list").find("versions")->size(); }
    json::Value routes() { return *exec("route.list").find("routes"); }
    void pointer(Pointer type, ui::Point p) {
        ui::PointerEvent e;
        e.type = type;
        e.button = 1;
        e.pos = p;
        root->pointer(e);
    }
    void drag(model::Id from, ui::Point to) {
        const auto before = history();
        const auto count = routes().size();
        pointer(Pointer::Down, centre(out(from).bounds()));
        pointer(Pointer::Move, to);
        root->frame(false);
        CHECK(history() == before);
        CHECK(routes().size() == count);
        REQUIRE(root->find("tcp.send-ghost"));
        pointer(Pointer::Up, to);
        sync();
    }
    void send(model::Id from, model::Id to) {
        json::Value args = json::Value::object();
        args["from"] = from.str();
        args["to"] = to.str();
        exec("route.add", std::move(args));
        sync();
    }
};
}  // namespace

TEST_CASE("TCP sends: output drag creates one default send and one undo step") {
    SendsFixture f;
    const auto before = f.history();
    CHECK(f.out(f.a).visible);
    CHECK(f.out(f.a).bounds().w == 28);
    CHECK(f.out(f.a).bounds().right() <= f.view().tcp().bounds().right());
    f.drag(f.a, f.target(f.b));
    const auto routes = f.routes();
    REQUIRE(routes.size() == 1);
    CHECK(routes.asArray()[0].find("from")->asString() == f.a.str());
    CHECK(routes.asArray()[0].find("to")->asString() == f.b.str());
    CHECK(routes.asArray()[0].find("tap")->asString() == "post_fader");
    CHECK(routes.asArray()[0].find("volume_db")->asDouble() == 0);
    CHECK(f.history() == before + 1);
    CHECK(f.errors.empty());
    f.root->key(SDLK_Z, ui::kCtrl);
    f.sync();
    CHECK(f.routes().size() == 0);
}

TEST_CASE("TCP sends: arrange lanes and automation lanes accept output drops") {
    SendsFixture f;
    auto before = f.history();
    f.drag(f.a, f.target(f.b, true));
    REQUIRE(f.routes().size() == 1);
    CHECK(f.routes().asArray()[0].find("to")->asString() == f.b.str());
    CHECK(f.history() == before + 1);
    f.root->key(SDLK_Z, ui::kCtrl);
    f.sync();
    json::Value args = json::Value::object();
    args["track"] = f.b.str();
    args["param"] = "volume";
    const auto result = f.exec("env.add", std::move(args));
    const auto env = model::Id::parse(result.find("env")->asString());
    REQUIRE(env);
    f.sync();
    const auto row = f.view().trackLayout().indexOfEnv(*env);
    REQUIRE(row);
    before = f.history();
    f.drag(f.a, SendsFixture::centre(f.view().rowRect(*row)));
    REQUIRE(f.routes().size() == 1);
    CHECK(f.routes().asArray()[0].find("to")->asString() == f.b.str());
    CHECK(f.history() == before + 1);
    CHECK(f.errors.empty());
}

TEST_CASE("TCP sends: own strip, empty space and outside targets do nothing") {
    SendsFixture f;
    const auto before = f.history();
    f.drag(f.a, f.target(f.a));
    f.drag(f.a, f.target(f.a, true));
    const auto tcp = f.view().tcp().bounds();
    f.drag(f.a, {tcp.x + 10, tcp.bottom() - 5});
    f.drag(f.a, {1200, f.target(f.b).y});
    f.drag(f.a, {tcp.x + 10, f.view().ruler().bounds().y + 10});
    CHECK(f.routes().size() == 0);
    CHECK(f.history() == before);
    CHECK(f.opened.empty());
    CHECK(f.errors.empty());
}

TEST_CASE("TCP sends: cycles and strip-type rules are reported without edits") {
    SendsFixture f(true);
    f.send(f.b, f.a);
    const auto before = f.history();
    f.drag(f.a, f.target(f.b));
    CHECK(f.routes().size() == 1);
    CHECK(f.history() == before);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors.back().find("route.cycle") != std::string::npos);
    const auto channel = f.add("channel", "C");
    f.sync();
    const auto n = f.history();
    f.drag(f.a, f.target(channel));
    CHECK(f.routes().size() == 1);
    CHECK(f.history() == n);
    REQUIRE(f.errors.size() == 2);
    CHECK(f.errors.back().find("route.strip_type") != std::string::npos);
}

TEST_CASE("TCP sends: a send into a child is refused by the parent path") {
    SendsFixture f(true);
    json::Value args = json::Value::object();
    args["track"] = f.b.str();
    args["parent"] = f.a.str();
    f.exec("track.move", std::move(args));
    f.sync();
    const auto before = f.history();
    f.drag(f.a, f.target(f.b, true));
    CHECK(f.routes().size() == 0);
    CHECK(f.history() == before);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors.back().find("route.cycle") != std::string::npos);
}

TEST_CASE("TCP sends: click anchors the routing callback; short rows hide output") {
    SendsFixture f;
    const auto before = f.history();
    const auto anchor = f.out(f.a).bounds();
    REQUIRE(f.root->click(f.out(f.a).id()));
    REQUIRE(f.opened.size() == 1);
    CHECK(f.opened[0].first == f.a);
    CHECK(f.opened[0].second.x == anchor.x);
    CHECK(f.opened[0].second.y == anchor.y);
    CHECK(f.opened[0].second.w == anchor.w);
    CHECK(f.history() == before);
    CHECK(f.routes().size() == 0);
    f.view().trackLayout().setTrackHeight(f.a, 30);
    f.root->relayout();
    f.root->frame(false);
    CHECK_FALSE(f.out(f.a).visible);
}

TEST_CASE("TCP sends: Escape cancels the overlay gesture and later drags still work") {
    SendsFixture f;
    const auto before = f.history();
    f.pointer(Pointer::Down, SendsFixture::centre(f.out(f.a).bounds()));
    f.pointer(Pointer::Move, f.target(f.b, true));
    f.root->frame(false);
    f.root->key(SDLK_ESCAPE);
    f.pointer(Pointer::Up, f.target(f.b, true));
    f.sync();
    CHECK(f.routes().size() == 0);
    CHECK(f.history() == before);
    f.drag(f.a, f.target(f.b));
    CHECK(f.routes().size() == 1);
    CHECK(f.errors.empty());
}

TEST_CASE("TCP sends: pooling during a captured drag keeps the original source") {
    SendsFixture f;
    model::Id destination = f.b;
    for (int i = 0; i < 12; ++i) destination = f.add("buss", "Extra " + std::to_string(i));
    f.sync();
    const auto before = f.history();
    f.pointer(Pointer::Down, SendsFixture::centre(f.out(f.a).bounds()));
    auto v = f.view().viewport();
    v.scrollY = 10000;
    f.view().setViewport(v);
    f.root->frame(false);
    CHECK(f.view().tcpPanel().rowNode(f.a) == nullptr);
    const auto target = f.target(destination, true);
    f.pointer(Pointer::Move, target);
    f.root->frame(false);
    CHECK(f.history() == before);
    f.pointer(Pointer::Up, target);
    f.sync();
    const auto routes = f.routes();
    REQUIRE(routes.size() == 1);
    CHECK(routes.asArray()[0].find("from")->asString() == f.a.str());
    CHECK(routes.asArray()[0].find("to")->asString() == destination.str());
    CHECK(f.history() == before + 1);
    CHECK(f.errors.empty());
}

TEST_CASE("TCP sends: overlay survives replacement of its owning view") {
    SendsFixture f;
    f.pointer(Pointer::Down, SendsFixture::centre(f.out(f.a).bounds()));
    f.pointer(Pointer::Move, f.target(f.b, true));
    f.root->frame(false);
    f.root->setContent(std::make_unique<ui::Node>("replacement"));
    f.root->frame(false);
    CHECK(f.routes().size() == 0);
}

TEST_CASE("TCP: the right-click menu renames, colours and deletes a track (one undo step each)") {
    SendsFixture f;
    const auto row = f.view().trackLayout().indexOf(f.a);
    REQUIRE(row);
    const ui::Rect r = f.view().tcpRowRect(*row);
    auto rightClick = [&] {
        ui::PointerEvent e;
        e.type = Pointer::Down;
        e.button = 3;
        e.pos = {r.x + 60, r.y + 10};
        f.root->pointer(e);
        e.type = Pointer::Up;
        f.root->pointer(e);
        f.root->frame(false);
    };
    // Menu rows are 26 px: Rename, Color…, Timebase…, (separator), Delete.
    auto pickRow = [&](const std::string& menu, int index) {
        ui::Node* m = f.root->find(menu);
        REQUIRE(m);
        const ui::Point p{m->bounds().x + 40, m->bounds().y + static_cast<float>(index) * 26 + 13};
        f.pointer(Pointer::Move, p);
        f.pointer(Pointer::Down, p);
        f.pointer(Pointer::Up, p);
        f.sync();
    };
    const std::string menu = "track.menu." + f.a.str();
    // Color… opens the swatches; teal is the fifth.
    rightClick();
    REQUIRE(f.root->find(menu));
    auto before = f.history();
    pickRow(menu, 1);
    REQUIRE(f.root->find("track.color." + f.a.str()));
    pickRow("track.color." + f.a.str(), 4);
    CHECK(f.app->head().tracks.find(f.a)->color == "teal");
    CHECK(f.history() == before + 1);
    // Rename edits the name in place.
    rightClick();
    pickRow(menu, 0);
    auto* field = dynamic_cast<ui::TextField*>(f.view().tcpPanel().controlNode(f.a, "rename"));
    REQUIRE(field);
    CHECK(field->visible);
    field->setText("Renamed");
    f.root->key(SDLK_RETURN);
    f.sync();
    CHECK(f.app->head().tracks.find(f.a)->name == "Renamed");
    // Timebase… (channels): what its items follow; "Follow time" is the third.
    before = f.history();
    rightClick();
    pickRow(menu, 2);
    REQUIRE(f.root->find("track.timebase." + f.a.str()));
    pickRow("track.timebase." + f.a.str(), 2);
    CHECK(f.app->head().tracks.find(f.a)->timebase == model::TrackTimebase::Time);
    CHECK(f.history() == before + 1);
    // Delete removes it.
    before = f.history();
    rightClick();
    pickRow(menu, 4);
    CHECK_FALSE(f.app->head().tracks.find(f.a));
    CHECK(f.history() == before + 1);
}

TEST_CASE("TCP: the fx button opens the plugin browser for an empty chain") {
    SendsFixture f;
    std::vector<std::string> added;
    f.actions->addUi("ui.fx.add", "FX: add plugin…", "track", [&](const json::Value& v) -> Status {
        added.push_back(v.asString());
        return {};
    });
    auto* fx = f.view().tcpPanel().controlNode(f.a, "fx");
    REQUIRE(fx);
    CHECK(fx->valueText() == "fx");
    REQUIRE(f.root->click(fx->id()));
    CHECK(added == std::vector<std::string>{f.a.str()});
}

TEST_CASE("TCP: the wheel over the tracks scrolls them and the lanes together, never a control") {
    SendsFixture f;
    std::vector<model::Id> tracks;
    for (int i = 0; i < 30; ++i) tracks.push_back(f.add("channel", "T" + std::to_string(i)));
    f.sync();
    auto wheel = [&](ui::Point p, float dy, unsigned mods = 0) {
        ui::PointerEvent e;
        e.type = Pointer::Wheel;
        e.pos = p;
        e.wheelY = dy;
        e.mods = mods;
        f.root->pointer(e);
        f.sync();
    };
    auto volumes = [&] {
        std::vector<double> v;
        for (const auto& t : f.app->head().tracks) v.push_back(t.value->volumeDb);
        return v;
    };
    // What the eye checks: every shown track row sits level with its lane row.
    auto aligned = [&] {
        int shown = 0;
        for (const model::Id& t : tracks) {
            ui::Node* row = f.view().tcpPanel().rowNode(t);
            if (!row) continue;
            const auto index = f.view().trackLayout().indexOf(t);
            REQUIRE(index);
            INFO("track " << t.str());
            CHECK(static_cast<double>(row->bounds().y) ==
                  doctest::Approx(static_cast<double>(f.view().rowRect(*index).y)));
            ++shown;
        }
        return shown;
    };
    const auto before = volumes();
    const ui::Rect tcp = f.view().tcpPanel().bounds();
    float last = f.view().viewport().scrollY;
    for (float fy : {0.15f, 0.3f, 0.45f, 0.6f, 0.75f})
        for (float fx : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 0.98f}) {
            wheel({tcp.x + tcp.w * fx, tcp.y + tcp.h * fy}, -1.0f);
            const float now = f.view().viewport().scrollY;
            INFO("at " << fx << "," << fy << " scroll " << last << " -> " << now);
            CHECK(now > last);
            CHECK(aligned() > 0);
            if (now > 600) wheel({tcp.x + 20, tcp.y + 40}, 30.0f);  // back to the top
            last = f.view().viewport().scrollY;
        }
    CHECK(volumes() == before);  // no fader or pan moved

    // Over the lanes the wheel zooms around the playhead, which keeps its place.
    f.view().setPlayhead(4.0);
    f.sync();
    const ui::Rect lanes = f.view().lanesRect();
    const float scroll = f.view().viewport().scrollY;
    const double pps = f.view().viewport().pixelsPerSecond;
    const float x = f.view().lanes().xAt(4.0);
    wheel({lanes.x + lanes.w / 2, lanes.y + lanes.h / 2}, 1.0f);
    CHECK(f.view().viewport().pixelsPerSecond > pps);
    CHECK(static_cast<double>(f.view().lanes().xAt(4.0)) ==
          doctest::Approx(static_cast<double>(x)).epsilon(0.001));
    CHECK(f.view().viewport().scrollY == scroll);
    // Ctrl scrolls the tracks from the lanes too, still aligned.
    wheel({lanes.x + lanes.w / 2, lanes.y + lanes.h / 2}, -1.0f, ui::kCtrl);
    CHECK(f.view().viewport().scrollY > scroll);
    CHECK(aligned() > 0);
}

TEST_CASE("TCP sends: output click opens the existing routing matrix action") {
    SendsFixture f;
    gui::RoutingMatrix matrix(*f.root, *f.actions, *f.dispatcher, *f.app);
    f.shell->onRouting = {};
    const auto before = f.history();
    REQUIRE(f.root->click(f.out(f.a).id()));
    f.root->frame(false);
    CHECK(matrix.isOpen());
    REQUIRE(f.root->find("routing"));
    CHECK(f.routes().size() == 0);
    CHECK(f.history() == before);
    f.root->key(SDLK_ESCAPE);
    CHECK_FALSE(matrix.isOpen());
}

TEST_CASE("the Delete key removes the selected items") {
    SendsFixture f;
    json::Value args = json::Value::object();
    args["track"] = f.a.str();
    args["notation"] = "voice a; 1: C4:w";
    const auto item = f.exec("item.add", std::move(args));
    const std::string id = item.find("item")->asString();
    f.state.selectedItems = {id};
    f.sync();
    f.root->key(SDLK_DELETE);
    f.sync();
    CHECK(f.app->head().items.size() == 0);
}
