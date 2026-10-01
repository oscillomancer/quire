// The app's actions, actions list and menus, driven by keys and clicks like a
// user would, asserting on the session through the bus (gui.md §10).
#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <unistd.h>
#include <cstdlib>
#include <filesystem>

#include "actions_list.h"
#include "agent_panel.h"
#include "app_actions.h"
#include "chrome.h"
#include "fake_llm.h"
#include "file_actions.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/ui/list.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "ui_persistence.h"

using namespace supple;

namespace {

struct App {
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
    bool ok = false;

    App() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-app-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "config");
        ::setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"app-test", 1000, 640, true});
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
        gui::registerGuiCommands(registry, ui);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
        root = std::make_unique<ui::Root>(*window, fonts, ui::Theme::dark());
        actions = std::make_unique<gui::AppActions>(registry, *dispatcher, *app, ui,
                                                    actions::Keymap::defaultFile("quire-test"));
        actions->addUi("ui.actions", "View: actions list",
                       [this] { gui::openActionsList(*root, *actions); });
        root->onUnhandledKey = [this](const ui::KeyEvent& e) {
            const bool typing = root->focused() && root->focused()->role() == ui::Role::TextField;
            return actions->key(e.key, e.mods, typing);
        };
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->add(gui::makeChrome(*root, *actions, gui::ChromeLayout::defaults()));
        frame->emplace<ui::Node>("body");
        root->setContent(std::move(frame));
        root->frame(false);
        ok = true;
    }
    ~App() {
        actions.reset();
        root.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    std::size_t tracks(std::string_view type = {}) const {
        std::size_t n = 0;
        for (const auto& e : app->head().tracks)
            n += type.empty() || model::stripTypeName(e.value->type) == type;
        return n;
    }
    void type(std::string_view text) {
        root->text(text);
        root->frame(false);
    }
    // SUPPLE_TEST_SNAPSHOTS=DIR writes what the test sees, for review.
    void snapshot(const char* name) {
        const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS");
        if (!out) return;
        root->damageAll();
        root->frame(false);
        (void)gui::writePng(std::filesystem::path(out) / name, root->readBack());
    }
    void key(const char* chord) {
        const auto c = actions::parseChord(chord);
        REQUIRE(c);
        root->key(c->key, c->mods);
        root->frame(false);
    }
};

}  // namespace

TEST_CASE("app actions: keys, the actions list, prompts, key binding and menus") {
    App a;
    if (!a.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    const std::size_t channels = a.tracks("channel");
    a.key("Ctrl+T");  // default binding → track.add:channel
    CHECK(a.tracks("channel") == channels + 1);

    // The actions list: search, Enter runs the top match.
    a.key("Shift+/");
    REQUIRE(a.root->popupOpen());
    auto* search = dynamic_cast<ui::TextField*>(a.root->find("actions.search"));
    REQUIRE(search);
    CHECK(a.root->focused() == search);
    a.type("add buss");
    a.snapshot("actions-list.png");
    a.key("Return");
    CHECK_FALSE(a.root->popupOpen());
    CHECK(a.tracks("buss") == 1);

    // Plain keys type into the search field instead of firing shortcuts.
    a.key("Shift+/");
    const std::size_t before = a.tracks();
    a.type("t");
    a.key("Ctrl+A");  // select all in the field, not a global action
    CHECK(a.tracks() == before);
    search = dynamic_cast<ui::TextField*>(a.root->find("actions.search"));
    CHECK(search->text() == "t");
    a.key("Escape");
    CHECK_FALSE(a.root->popupOpen());

    // An action with a prompted argument opens a form; JSON values parse.
    a.key("Shift+/");
    a.type("tempo set");
    a.key("Return");
    REQUIRE(a.root->popupOpen());
    auto* points = dynamic_cast<ui::TextField*>(a.root->find("actions.prompt.points"));
    REQUIRE(points);
    a.type(R"([{"beat": 0, "bpm": 90}])");
    a.key("Return");
    CHECK_FALSE(a.root->popupOpen());
    CHECK(a.app->head().timeMap->bpmAt(0) == doctest::Approx(90));

    // Bind a key from the list (Ctrl+B, then the chord); it is saved and works.
    a.key("Shift+/");
    a.type("add master");
    a.key("Ctrl+B");
    a.key("F5");
    CHECK(a.actions->keysFor("track.add:master") == "F5");
    a.key("Escape");
    CHECK(std::filesystem::exists(a.dir / "config" / "quire-test" / "keymap.json"));
    const std::size_t masters = a.tracks("master");
    a.key("F5");
    CHECK(a.tracks("master") == masters + 1);

    // Menus: click a title, pick an item with the keyboard.
    REQUIRE(a.root->click("menu.Track"));
    a.root->frame(false);
    REQUIRE(a.root->popupOpen());
    a.snapshot("menu.png");
    a.key("Down");  // nothing selected yet → Add channel
    a.key("Down");  // → Add buss
    a.key("Return");
    CHECK(a.tracks("buss") == 2);
    // Toolbar buttons run their actions.
    const std::size_t all = a.tracks();
    REQUIRE(a.root->click("tool.track.add:channel"));
    CHECK(a.tracks() == all + 1);

    // Agents can read the selection.
    a.ui.selectedTracks = {"tr_1"};
    auto sel = a.dispatcher->execute(
        {"gui.selection", json::Value::object(), {cmd::Origin::Type::Test, "t", {}}});
    REQUIRE(sel);
    CHECK((*sel)["tracks"].asArray().size() == 1);
}

TEST_CASE("the agent panel: a request streams in, the change is reviewed, A/B'd and accepted") {
    App a;
    if (!a.ok) return;
    using namespace supple::agent::testing;
    FakeServer server;
    server.push(anthropicTurn(
        "Adding a drum buss.",
        {{"exec", R"({"command": "track.add", "args": {"type": "buss", "name": "Drums"}})"}}));
    server.push(anthropicTurn("Done: one buss named Drums.", {}));
    std::vector<std::function<void()>> posted;
    std::mutex m;
    gui::AgentPanel panel(
        *a.root, *a.app, a.registry, *a.dispatcher,
        [&](std::function<void()> f) {
            std::lock_guard lock(m);
            posted.push_back(std::move(f));
        },
        a.dir / "config" / "agent.json", SUPPLE_SKILL_DIR);
    panel.config().endpoints = {
        {agent::Endpoint::Kind::Anthropic, "fake", server.url(), "test-model", "k"}};
    auto frame = std::make_unique<ui::Node>("window");
    frame->layout.dir = ui::Layout::Dir::Row;
    frame->emplace<ui::Node>("body");
    frame->add(panel.build());
    a.root->setContent(std::move(frame));
    panel.toggle();
    a.root->frame(false);
    CHECK(a.root->focused() == a.root->find("agent.input"));
    a.type("Add a drum buss");
    a.key("Return");
    REQUIRE(panel.session());
    for (int i = 0; i < 2000 && panel.session()->busy(); ++i) {
        std::vector<std::function<void()>> now;
        {
            std::lock_guard lock(m);
            now.swap(posted);
        }
        for (auto& f : now) f();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    {  // the idle notification
        std::lock_guard lock(m);
        for (auto& f : posted) f();
        posted.clear();
    }
    REQUIRE_FALSE(panel.session()->busy());
    a.root->frame(false);
    auto* diff = dynamic_cast<ui::TextBlock*>(a.root->find("agent.review.diff"));
    REQUIRE(diff);
    CHECK(a.root->find("agent.review")->visible);
    CHECK(diff->text() == "+ buss \"Drums\"");  // the master's implied "children" update is hidden
    a.snapshot("agent-panel.png");
    CHECK(a.tracks("buss") == 1);
    REQUIRE(a.root->click("agent.yours"));
    CHECK(a.tracks("buss") == 0);
    REQUIRE(a.root->click("agent.theirs"));
    CHECK(a.tracks("buss") == 1);
    REQUIRE(a.root->click("agent.accept"));
    a.root->frame(false);
    CHECK(a.tracks("buss") == 1);
    CHECK_FALSE(a.root->find("agent.review")->visible);
    CHECK(panel.session()->branch().empty());
}

TEST_CASE("UI state is saved beside the project and restored when it is opened") {
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-ui-state-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto project = dir / "song.json";
    double zoom = 40;
    bool panel = false;
    auto make = [&](gui::UiPersistence& p) {
        p.add(
            "view",
            [&] {
                json::Value v = json::Value::object();
                v["zoom"] = zoom;
                return v;
            },
            [&](const json::Value& v) { zoom = v.find("zoom")->asDouble(); });
        p.add(
            "panels",
            [&] {
                json::Value v = json::Value::object();
                v["agent"] = panel;
                return v;
            },
            [&](const json::Value& v) { panel = v.find("agent")->asBool(); });
    };
    {
        gui::UiPersistence p;
        make(p);
        p.tick(project);  // nothing on disk yet
        zoom = 160;
        panel = true;
        REQUIRE(p.flush(project));
        CHECK(std::filesystem::exists(dir / "song.ui.json"));
        p.tick({});  // an unsaved project writes nothing
    }
    zoom = 1;
    panel = false;
    {
        gui::UiPersistence p;
        make(p);
        p.tick(project);  // opening the project restores its UI state
        CHECK(zoom == 160);
        CHECK(panel);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE(
    "file actions: dirty tracking, save as, import (one undo step), confirm before discarding, "
    "open") {
    App a;
    if (!a.ok) return;
    std::vector<std::function<void()>> posted;
    gui::FileActions files(*a.root, *a.actions, *a.dispatcher, *a.app, a.ui, a.window->sdl(),
                           [&](std::function<void()> f) { posted.push_back(std::move(f)); });
    CHECK_FALSE(files.dirty());
    a.key("Ctrl+T");
    CHECK(files.dirty());
    files.saveAs(a.dir / "song");  // the extension is added
    CHECK(std::filesystem::exists(a.dir / "song.json"));
    CHECK_FALSE(files.dirty());

    // Import: one file onto the selected track; several onto new tracks, one undo step.
    audio::AudioData tone;
    tone.channels.assign(2, std::vector<float>(48000, 0.25f));
    for (const char* name : {"a.wav", "b.wav", "c.wav"})
        REQUIRE(
            writeFileAtomic(a.dir / name, std::span<const std::uint8_t>(*audio::encodeWav(tone))));
    const std::string track = a.app->head().tracks.begin()[1].id.str();  // the channel Ctrl+T added
    a.ui.selectedTracks = {track};
    files.import({a.dir / "a.wav"});
    std::size_t onTrack = 0;
    for (const auto& e : a.app->head().items) onTrack += e.value->track.str() == track;
    CHECK(onTrack == 1);
    const std::size_t tracks = a.tracks();
    files.import({a.dir / "b.wav", a.dir / "c.wav"});
    CHECK(a.tracks() == tracks + 2);
    a.key("Ctrl+Z");
    CHECK(a.tracks() == tracks);  // both imports undone together

    // Unsaved changes ask first; Cancel keeps them, Don't save goes ahead.
    CHECK(files.dirty());
    bool went = false;
    files.confirmDiscard("Open another project?", [&] { went = true; });
    a.root->frame(false);
    REQUIRE(a.root->popupOpen());
    REQUIRE(a.root->click("file.confirm.cancel"));
    CHECK_FALSE(went);
    files.confirmDiscard("Open another project?", [&] { went = true; });
    a.root->frame(false);
    REQUIRE(a.root->click("file.confirm.discard"));
    CHECK(went);

    files.openPath(a.dir / "song.json");
    CHECK_FALSE(files.dirty());
    CHECK(a.app->projectPath() == a.dir / "song.json");
    CHECK(a.app->head().items.size() == 0);  // saved before the imports
}
