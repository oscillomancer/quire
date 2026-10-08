// The app's actions, actions list and menus, driven by keys and clicks like a
// user would, asserting on the session through the bus (gui.md §10).
#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include "product_fixture.h"

#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "actions_list.h"
#include "agent_panel.h"
#include "app_actions.h"
#include "appearance.h"
#include "chrome.h"
#include "fake_llm.h"
#include "file_actions.h"
#include "history_panel.h"
#include "mixer.h"
#include "mouse_dialog.h"
#include "screensets.h"
#include "shell.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/list.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "transport_bar.h"
#include "ui_persistence.h"

#include <stb_image.h>

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
        actions = std::make_unique<gui::AppActions>(
            registry, *dispatcher, *app, ui, actions::Keymap::defaultFile(gui::test::slug()));
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
    CHECK(std::filesystem::exists(a.dir / "config" / gui::test::slug() / "keymap.json"));
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

TEST_CASE("the toolbar and menus are edited in the app and saved (gap log #37)") {
    App a;
    if (!a.ok) return;
    const auto file = a.dir / "config" / "menus.json";
    gui::Chrome chrome(*a.root, *a.actions, gui::ChromeLayout::defaults(), file);
    {
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->add(chrome.build());
        frame->emplace<ui::Node>("body");
        a.root->setContent(std::move(frame));
        a.root->frame(false);
    }
    CHECK_FALSE(a.root->find("tool.track.add:buss"));
    // The transport bar has these; the top row keeps only what it does not.
    CHECK_FALSE(a.root->find("tool.transport.toggle"));
    CHECK_FALSE(a.root->find("tool.transport.record"));
    CHECK_FALSE(a.root->find("tool.transport.loop.toggle"));
    CHECK(a.root->find("tool.history.undo"));
    CHECK(a.root->find("tool.ui.actions"));
    // Ctrl+T in the actions list puts the action on the toolbar.
    gui::openActionsList(*a.root, *a.actions, &chrome);
    a.type("add buss");
    a.key("Ctrl+T");
    a.key("Escape");
    REQUIRE(a.root->find("tool.track.add:buss"));
    const std::size_t busses = a.tracks("buss");
    REQUIRE(a.root->click("tool.track.add:buss"));
    CHECK(a.tracks("buss") == busses + 1);
    // Ctrl+M lists the menus; picking Edit adds it there.
    gui::openActionsList(*a.root, *a.actions, &chrome);
    a.type("add buss");
    a.key("Ctrl+M");
    REQUIRE(a.root->find("actions.menus"));
    a.key("Down");  // File
    a.key("Down");  // Edit
    a.key("Return");
    CHECK(chrome.inMenu("Edit", "track.add:buss"));
    a.key("Escape");
    // A right-click on a toolbar button moves or removes it.
    REQUIRE(a.root->click("tool.track.add:buss", 3));
    a.root->frame(false);
    REQUIRE(a.root->find("tool.menu"));
    a.key("Down");  // Move left
    a.key("Return");
    const auto& t = chrome.layout().toolbar;
    REQUIRE(t.size() >= 2);
    CHECK(t[t.size() - 2] == "track.add:buss");
    REQUIRE(a.root->click("tool.track.add:buss", 3));
    a.root->frame(false);
    a.key("Down");
    a.key("Down");
    a.key("Down");  // Remove from toolbar
    a.key("Return");
    a.root->frame(false);
    CHECK_FALSE(a.root->find("tool.track.add:buss"));
    // Every edit was saved; the file reads back as the chrome now is.
    auto loaded = gui::ChromeLayout::load(file);
    REQUIRE(loaded);
    CHECK(loaded->toolbar == chrome.layout().toolbar);
    CHECK(loaded->toJson() == chrome.layout().toJson());
}

TEST_CASE("the mouse modifiers dialog edits the live map and saves it (gap log #37)") {
    App a;
    if (!a.ok) return;
    views::MouseMap map;
    const auto file = a.dir / "config" / "mouse.json";
    gui::MouseDialog dialog(*a.root, map, file);
    dialog.open("item.drag");
    a.root->frame(false);
    REQUIRE(a.root->find("mouse.set.alt"));
    CHECK(a.root->find("mouse.set.alt")->valueText() == "slip");
    CHECK(a.root->find("mouse.set.ctrl+alt")->valueText() == "move  (plain)");
    // Alt: pick "copy" (move, move off the grid, copy, ...).
    REQUIRE(a.root->click("mouse.set.alt"));
    a.root->frame(false);
    REQUIRE(a.root->find("mouse.behaviours"));
    a.key("Down");
    a.key("Down");
    a.key("Down");
    a.key("Return");
    CHECK(map.behaviour("item.drag", ui::kAlt) == "copy");
    CHECK(a.root->find("mouse.set.alt")->valueText() == "copy");
    auto saved = views::MouseMap::load(file);
    REQUIRE(saved);
    CHECK(*saved == map);
    // Another context from the picker; then reset puts the defaults back.
    REQUIRE(a.root->click("mouse.reset"));
    CHECK(map.behaviour("item.drag", ui::kAlt) == "slip");
    dialog.open("envelope");
    a.root->frame(false);
    CHECK(a.root->find("mouse.set.alt")->valueText() == "delete");
    // The wheel's zoom anchor is one setting: only the plain row, default the project cursor.
    dialog.open("wheel.zoom");
    a.root->frame(false);
    CHECK_FALSE(a.root->find("mouse.set.alt"));
    REQUIRE(a.root->find("mouse.set."));
    CHECK(a.root->find("mouse.set.")->valueText().find("project cursor") != std::string::npos);
    dialog.set("wheel.zoom", 0, "mouse_pointer");
    CHECK(map.behaviour("wheel.zoom", 0) == "mouse_pointer");
    CHECK(a.root->find("mouse.set.")->valueText() == "the mouse pointer");
}

TEST_CASE("native dialog completions respect shutdown before posting and running") {
    int posts = 0, completions = 0;
    std::vector<std::function<void()>> queued;
    auto lifetime = std::make_unique<gui::DialogLifetime>();
    auto complete = lifetime->guard(
        [&](auto task) {
            ++posts;
            queued.push_back(std::move(task));
        },
        [&](auto) { ++completions; });
    SUBCASE("shutdown before the native callback") {
        lifetime.reset();
        complete({"example.json"});
        CHECK(posts == 0);
    }
    SUBCASE("shutdown after posting") {
        complete({"example.json"});
        REQUIRE(queued.size() == 1);
        lifetime.reset();
        queued.front()();
        CHECK(completions == 0);
    }
    SUBCASE("live completion") {
        complete({"example.json"});
        REQUIRE(queued.size() == 1);
        queued.front()();
        CHECK(completions == 1);
    }
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
    REQUIRE(a.root->click("agent.send"));
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
    const auto requests = server.requests();
    REQUIRE_FALSE(requests.empty());
    REQUIRE(requests.front().find("messages"));
    const auto* content = requests.front().find("messages")->asArray().back().find("content");
    REQUIRE(content);
    REQUIRE_FALSE(content->asArray().empty());
    CHECK(content->asArray().front().find("text")->asString().ends_with("\n\nAdd a drum buss"));
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

TEST_CASE("agent settings recover an out-of-range current endpoint") {
    App a;
    if (!a.ok) return;
    const auto file = a.dir / "config" / "agent.json";
    agent::Config config;
    config.endpoints.push_back({agent::Endpoint::Kind::OpenAI, "existing", "http://localhost/v1",
                                "existing-model", "existing-key"});
    config.current = 42;
    REQUIRE(config.save(file));
    gui::AgentPanel panel(
        *a.root, *a.app, a.registry, *a.dispatcher, [](auto) {}, file, SUPPLE_SKILL_DIR);
    auto frame = std::make_unique<ui::Node>("window");
    frame->layout.dir = ui::Layout::Dir::Row;
    frame->add(panel.build());
    a.root->setContent(std::move(frame));
    panel.openSettings();
    auto* model = dynamic_cast<ui::TextField*>(a.root->find("agent.settings.model"));
    REQUIRE(model);
    model->setText("new-model");
    a.root->frame(false);
    REQUIRE(a.root->click("agent.settings.save"));
    const auto loaded = agent::Config::load(file);
    REQUIRE(loaded);
    REQUIRE(loaded->active());
    CHECK(loaded->active()->model == "new-model");
    REQUIRE(loaded->endpoints.size() == 2);
    CHECK(loaded->endpoints.front().model == "existing-model");
    CHECK(loaded->endpoints.front().apiKey == "existing-key");
}

TEST_CASE("hidden docks release their own focus and preserve focus elsewhere") {
    App a;
    if (!a.ok) return;
    gui::AgentPanel agentPanel(
        *a.root, *a.app, a.registry, *a.dispatcher, [](auto) {}, a.dir / "config" / "agent.json",
        SUPPLE_SKILL_DIR);
    gui::HistoryPanel history(*a.root, *a.actions, *a.dispatcher, *a.app);
    gui::MeterFeed feed;
    gui::Mixer mixer(*a.root, *a.actions, *a.dispatcher, *a.app, feed);
    auto frame = std::make_unique<ui::Node>("window");
    auto& outside = frame->emplace<ui::TextField>("outside");
    frame->add(agentPanel.build());
    frame->add(history.build());
    frame->add(mixer.build());
    a.root->setContent(std::move(frame));
    auto check = [&](auto& dock, const char* id) {
        dock.setVisible(true);
        a.root->frame(false);
        auto* target = a.root->find(id);
        REQUIRE(target);
        a.root->focus(target);
        dock.setVisible(false);
        CHECK(a.root->focused() == nullptr);
        if (auto* input = dynamic_cast<ui::TextField*>(target)) {
            const auto before = input->text();
            a.root->text("hidden");
            CHECK(input->text() == before);
        }
        dock.setVisible(true);
        a.root->focus(&outside);
        dock.setVisible(false);
        CHECK(a.root->focused() == &outside);
    };
    check(agentPanel, "agent.input");
    check(history, "history.newname");
    check(mixer, "mixer.close");
    mixer.setVisible(true);
    a.root->frame(false);
    auto* name = a.root->find("mixer.nameedit.tr_0");
    REQUIRE(name);
    REQUIRE(name->onPointer({ui::PointerEvent::Type::Down, {}, 1, 2}));
    auto* rename = dynamic_cast<ui::TextField*>(a.root->find("mixer.rename.tr_0"));
    REQUIRE(rename);
    REQUIRE(a.root->focused() == rename);
    mixer.setVisible(false);
    CHECK(a.root->focused() == nullptr);
    CHECK_FALSE(rename->visible);
}

TEST_CASE("the appearance: warm and dark by default, saved and read back, palettes differ") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-appearance-" + std::to_string(::getpid()));
    const auto file = dir / "appearance.json";
    gui::Appearance look = gui::Appearance::load(file);  // missing: the defaults
    CHECK(look.dark);
    CHECK(look.palette == ui::Palette::Warm);
    CHECK(look.theme().name() == "dark-warm");
    look.dark = false;
    look.palette = ui::Palette::Ink;
    REQUIRE(look.save(file));
    const gui::Appearance read = gui::Appearance::load(file);
    CHECK_FALSE(read.dark);
    CHECK(read.palette == ui::Palette::Ink);
    CHECK(read.theme().name() == "light");
    // Warm swaps the accent family; strip types and track colours stay.
    const ui::Theme warm = ui::Theme::dark(ui::Palette::Warm), ink = ui::Theme::dark();
    CHECK_FALSE(warm.color("accent") == ink.color("accent"));
    CHECK(warm.color("accent").r > warm.color("accent").b);  // orange
    CHECK(warm.color("channel") == ink.color("channel"));
    CHECK(warm.trackColor("teal") == ink.trackColor("teal"));
    CHECK(warm.trackColor("#102030") == ui::Color::hex(0x102030));
    CHECK_FALSE(warm.trackColor("pink"));
    CHECK_FALSE(warm.trackColor(""));
    std::filesystem::remove_all(dir);
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

TEST_CASE("UI state written in the background never overtakes a later flush") {
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-ui-async-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto project = dir / "song.json";
    double zoom = 40;
    {
        gui::UiPersistence p;
        p.add(
            "view",
            [&] {
                json::Value v = json::Value::object();
                v["zoom"] = zoom;
                return v;
            },
            [&](const json::Value& v) { zoom = v.find("zoom")->asDouble(); });
        p.tick(project);
        zoom = 80;
        // The periodic write (at most once a second) goes to the writer thread.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        p.tick(project);
        zoom = 120;
        REQUIRE(p.flush(project));  // after the pending write: the newest state wins
    }
    auto bytes = readFile(dir / "song.ui.json");
    REQUIRE(bytes);
    const auto doc = json::parse(std::string(bytes->begin(), bytes->end()));
    REQUIRE(doc);
    CHECK(doc->find("view")->find("zoom")->asDouble() == 120);
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
    files.saveAs(a.dir / "song");  // the extension is added, in a folder of its own
    CHECK(std::filesystem::exists(a.dir / "song" / "song.json"));
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

    files.openPath(a.dir / "song" / "song.json");
    CHECK_FALSE(files.dirty());
    CHECK(a.app->projectPath() == a.dir / "song" / "song.json");
    CHECK(a.app->head().items.size() == 0);  // saved before the imports
}

TEST_CASE("save as gives each project a folder of its own") {
    namespace fs = std::filesystem;
    const fs::path home = "/home/u/Documents/media";
    CHECK(gui::projectFolderPath(home / "Song", {}) == home / "Song" / "Song.json");
    CHECK(gui::projectFolderPath(home / "Song.json", {}) == home / "Song" / "Song.json");
    // Inside the project's own folder already: as chosen.
    CHECK(gui::projectFolderPath(home / "Song" / "Song.json", {}) == home / "Song" / "Song.json");
    App a;
    if (!a.ok) return;
    gui::FileActions files(*a.root, *a.actions, *a.dispatcher, *a.app, a.ui, a.window->sdl(),
                           [](std::function<void()> f) { f(); });
    files.saveAs(a.dir / "Song");
    REQUIRE(a.app->projectPath() == a.dir / "Song" / "Song.json");
    // A new name chosen from inside it goes beside it, not inside.
    CHECK(gui::projectFolderPath(a.dir / "Song" / "Mix", a.app->projectPath()) ==
          a.dir / "Mix" / "Mix.json");
    files.saveAs(a.dir / "Song" / "Mix.json");
    CHECK(a.app->projectPath() == a.dir / "Mix" / "Mix.json");
    // Saving as onto another existing project is refused.
    std::string error;
    a.actions->onError = [&](const std::string& message) { error = message; };
    files.saveAs(a.dir / "Song");
    CHECK(error.find("already exists") != std::string::npos);
    CHECK(a.app->projectPath() == a.dir / "Mix" / "Mix.json");
}

TEST_CASE("file actions: project templates save, start a new project and delete") {
    App a;
    if (!a.ok) return;
    gui::FileActions files(*a.root, *a.actions, *a.dispatcher, *a.app, a.ui, a.window->sdl(),
                           [](std::function<void()> f) { f(); });
    const auto dir = a.dir / "config" / std::string(product::kSlug) / "templates";
    a.key("Ctrl+T");
    const std::size_t tracks = a.tracks();
    // File > Save template…: a name, then Save. A taken name asks once more.
    REQUIRE(a.actions->run("ui.file.template.save"));
    a.root->frame(false);
    CHECK(a.root->focused()->id() == "file.template.name");
    a.root->text("Band");
    REQUIRE(a.root->click("file.template.commit"));
    CHECK(std::filesystem::exists(dir / "Band" / "project.json"));
    CHECK_FALSE(a.root->popupOpen());
    CHECK(a.app->projectPath().empty());
    REQUIRE(a.actions->run("ui.file.template.save"));
    a.root->frame(false);
    a.root->text("Band");
    REQUIRE(a.root->click("file.template.commit"));
    REQUIRE(a.root->popupOpen());
    auto* status = dynamic_cast<ui::Label*>(a.root->find("file.template.status"));
    REQUIRE(status);
    CHECK(status->text() == "\"Band\" exists. Save again to replace it.");
    REQUIRE(a.root->click("file.template.commit"));
    CHECK_FALSE(a.root->popupOpen());
    files.saveAs(a.dir / "other");  // keeps the template untouched
    REQUIRE(a.actions->run("ui.file.template.save"));
    a.root->frame(false);
    a.root->text("Spare");
    a.root->key(SDLK_RETURN);

    // File > Load template…: a click starts a new, unsaved project from it.
    a.key("Ctrl+T");
    CHECK(a.tracks() == tracks + 1);
    REQUIRE(a.actions->run("ui.file.template.load"));
    a.root->frame(false);
    auto* list = dynamic_cast<ui::List*>(a.root->find("file.template.list"));
    REQUIRE(list);
    CHECK(list->count() == 2);
    a.root->frame(false);
    const ui::Rect r = list->bounds();
    const ui::Point first{r.x + 10, r.y + 8};
    a.root->pointer({ui::PointerEvent::Type::Down, first, 1});
    a.root->pointer({ui::PointerEvent::Type::Up, first, 1});
    a.root->frame(false);
    REQUIRE(a.root->click("file.confirm.discard"));  // the unsaved Ctrl+T
    CHECK(a.tracks() == tracks);
    CHECK(a.app->projectPath().empty());
    CHECK_FALSE(files.dirty());

    // A right click deletes one (here "Band", the first).
    REQUIRE(a.actions->run("ui.file.template.load"));
    a.root->frame(false);
    a.root->pointer({ui::PointerEvent::Type::Down, first, 3});
    a.root->pointer({ui::PointerEvent::Type::Up, first, 3});
    a.root->frame(false);
    REQUIRE(a.root->click("file.template.context"));  // its one item, Delete
    a.root->frame(false);
    CHECK_FALSE(std::filesystem::exists(dir / "Band" / "project.json"));
    CHECK(std::filesystem::exists(dir / "Spare" / "project.json"));
    list = dynamic_cast<ui::List*>(a.root->find("file.template.list"));
    REQUIRE(list);
    CHECK(list->count() == 1);
}

TEST_CASE("actions list: keys after a click on a row, overriding defaults, and a reset") {
    App a;
    if (!a.ok) return;
    gui::Chrome chrome(*a.root, *a.actions, gui::ChromeLayout::defaults(), {});
    auto clickFirstRow = [&] {
        a.root->frame(false);
        auto* list = a.root->find("actions.results");
        REQUIRE(list);
        const ui::Point p{list->bounds().x + 40, list->bounds().y + 10};
        a.root->pointer({ui::PointerEvent::Type::Down, p, 1});
        a.root->pointer({ui::PointerEvent::Type::Up, p, 1});
        a.root->frame(false);
        REQUIRE(a.root->focused() == list);  // the click took the focus off the search
    };
    // Ctrl+B with the list focused: bind F6.
    gui::openActionsList(*a.root, *a.actions, &chrome);
    a.type("add master");
    clickFirstRow();
    a.key("Ctrl+B");
    a.key("F6");
    CHECK(a.actions->keysFor("track.add:master") == "F6");
    // Typing with the list focused goes on searching.
    a.type("x");
    CHECK(dynamic_cast<ui::TextField*>(a.root->find("actions.search"))->text() == "add masterx");
    a.key("Escape");
    // Ctrl+M with the list focused lists the menus.
    gui::openActionsList(*a.root, *a.actions, &chrome);
    a.type("add buss");
    clickFirstRow();
    a.key("Ctrl+M");
    CHECK(a.root->find("actions.menus"));
    a.root->closePopups();
    // A key an arrange default uses (S splits items) goes to the new action,
    // in the arrange view too.
    gui::openActionsList(*a.root, *a.actions, &chrome);
    a.type("add master");
    a.key("Ctrl+B");
    a.key("S");
    a.key("Escape");
    CHECK(a.actions->keysFor("item.split:playhead").empty());
    const std::size_t masters = a.tracks("master");
    CHECK(a.actions->key(SDLK_S, 0, false, {"arrange", "global"}));
    CHECK(a.tracks("master") == masters + 1);
    // Reset keys…: asks first; Cancel keeps them, Reset restores the defaults.
    gui::openActionsList(*a.root, *a.actions, &chrome);
    a.root->frame(false);  // a new popup has bounds after a frame
    REQUIRE(a.root->click("actions.keys.reset"));
    a.root->frame(false);
    REQUIRE(a.root->find("keys.reset"));
    REQUIRE(a.root->click("keys.reset.cancel"));
    CHECK(a.actions->keysFor("track.add:master") == "F6");
    REQUIRE(a.root->click("actions.keys.reset"));
    a.root->frame(false);
    REQUIRE(a.root->click("keys.reset.confirm"));
    CHECK(a.actions->keysFor("track.add:master").empty());
    CHECK(a.actions->keysFor("item.split:playhead") == "S");
    auto saved = actions::Keymap::load(a.actions->keymapFile());
    REQUIRE(saved);
    CHECK(saved->bindings() == actions::Keymap::defaults().bindings());
    CHECK(a.root->find("actions"));  // the list stays open
}

TEST_CASE("an action binds the selected tracks ($selection.track)") {
    App a;
    if (!a.ok) {
        MESSAGE("skipped: no offscreen GL in this environment");
        return;
    }
    json::Value add = json::Value::object();
    add["name"] = "Doomed";
    auto added = a.dispatcher->execute(
        {"track.add", std::move(add), cmd::Origin{cmd::Origin::Type::Ui, "gui", {}}});
    REQUIRE(added);
    const std::string track = (*added)["track"].asString();
    a.ui.selectedTracks = {track};  // the TCP puts the clicked strip here
    // track.remove's required argument is named `track`, so the registry binds
    // it to $selection.track: running the action removes what is selected.
    auto run = a.actions->run("track.remove");
    REQUIRE_MESSAGE(run, (run ? "" : run.error().toString()));
    CHECK(a.app->head().tracks.find(*model::Id::parse(track)) == nullptr);
}

TEST_CASE("the transport bar drives the transport, the tempo and the meter (P5.5)") {
    App a;
    if (!a.ok) {
        MESSAGE("skipped: no offscreen GL in this environment");
        return;
    }
    const cmd::Origin ui{cmd::Origin::Type::Ui, "gui", {}};
    json::Value start = json::Value::object();
    start["backend"] = "dummy";  // no device: the transport still moves
    start["block"] = 256;
    REQUIRE(a.dispatcher->execute({"engine.start", std::move(start), ui}));
    auto bar = std::make_unique<gui::TransportBar>(*a.app, *a.actions, *a.dispatcher, *a.root);
    auto frame = std::make_unique<ui::Node>("frame");
    frame->layout.dir = ui::Layout::Dir::Column;
    frame->add(bar->build());
    frame->emplace<ui::Node>("below").layout.height = ui::SizeRule::fixed(40);  // a rule below
    a.root->setContent(std::move(frame));
    bar->sync();
    a.root->frame(false);
    auto get = [&] {
        auto r = a.dispatcher->execute({"transport.get", json::Value::object(), ui});
        REQUIRE(r);
        return *r;
    };
    CHECK_FALSE(get()["playing"].asBool());
    bar->sync();
    a.root->frame(false);
    CHECK(a.root->find("transport.position")->valueText() == "1.1.000");
    a.root->click("transport.play");
    CHECK(get()["playing"].asBool());
    // Recording needs an armed track with an input.
    json::Value add = json::Value::object();
    add["name"] = "Rec";
    auto added = a.dispatcher->execute({"track.add", std::move(add), ui});
    REQUIRE(added);
    const std::string track = (*added)["track"].asString();
    json::Value arm = json::Value::object();
    arm["track"] = track;
    arm["armed"] = true;
    json::Value input = json::Value::array();
    input.push_back(1);
    input.push_back(2);
    arm["record_input"] = std::move(input);
    REQUIRE(a.dispatcher->execute({"track.set", std::move(arm), ui}));
    // An armed track: the bar says where the recording will go, before it starts.
    std::this_thread::sleep_for(std::chrono::milliseconds(260));  // the ≤ 4 Hz readouts
    bar->sync();
    const std::string recPath = a.root->find("transport.recpath")->valueText();
    CHECK(recPath.rfind("rec: ", 0) == 0);
    CHECK(recPath.find("/rec", 5) != std::string::npos);
    // The count-in button cycles none, 1, 2, 4 bars (transport.metronome).
    CHECK(a.root->find("transport.count")->valueText() == "count");
    a.root->click("transport.count");
    CHECK(a.app->metronome().countInBars == 1);
    CHECK(a.root->find("transport.count")->valueText() == "count 1");
    for (int expected : {2, 4, 0}) {
        a.root->click("transport.count");
        CHECK(a.app->metronome().countInBars == expected);
    }
    a.root->click("transport.record");
    CHECK(get()["recording"].asBool());
    // Loop and punch need a range before they can be switched on (the bar only
    // toggles them; the ruler drag that sets a range is P5.6).
    json::Value loopRange = json::Value::object();
    loopRange["start_seconds"] = 0;
    loopRange["end_seconds"] = 4;
    REQUIRE(a.dispatcher->execute({"transport.loop", std::move(loopRange), ui}));
    const bool loopBefore = get()["loop"]["enabled"].asBool();
    a.root->click("transport.loop");
    CHECK(get()["loop"]["enabled"].asBool() != loopBefore);  // the button toggles
    json::Value punchRange = json::Value::object();
    punchRange["start_seconds"] = 0;
    punchRange["end_seconds"] = 2;
    REQUIRE(a.dispatcher->execute({"transport.punch", std::move(punchRange), ui}));
    const bool punchBefore = get()["punch"]["enabled"].asBool();
    a.root->click("transport.punch");
    CHECK(get()["punch"]["enabled"].asBool() != punchBefore);
    bar->sync();
    a.root->frame(false);
    // The tempo form: click the button, type, Set (the popup closes).
    a.root->click("transport.tempo");
    a.root->frame(false);  // the popup is arranged before anything in it is hit
    auto* field = static_cast<ui::TextField*>(a.root->find("tempo.field"));
    REQUIRE(field);
    field->setText("90");
    a.root->click("tempo.set");
    REQUIRE(a.app->head().timeMap->tempo().size() == 1);
    CHECK(a.app->head().timeMap->tempo()[0].bpm == doctest::Approx(90.0));
    CHECK(a.root->find("tempo.field") == nullptr);  // closed
    // The meter form, the same way.
    a.root->click("transport.meter");
    a.root->frame(false);
    auto* mfield = static_cast<ui::TextField*>(a.root->find("meter.field"));
    REQUIRE(mfield);
    mfield->setText("3/4");
    a.root->click("meter.set");
    CHECK(a.app->head().timeMap->meterAt(1).numerator == 3);
    bar->sync();
    a.root->frame(false);
    // Stop and disarm: the golden must not show this run's temporary folder.
    a.root->click("transport.play");
    CHECK_FALSE(get()["recording"].asBool());
    json::Value disarm = json::Value::object();
    disarm["track"] = track;
    disarm["armed"] = false;
    REQUIRE(a.dispatcher->execute({"track.set", std::move(disarm), ui}));
    json::Value home = json::Value::object();
    home["seconds"] = 0.0;
    REQUIRE(a.dispatcher->execute({"transport.seek", std::move(home), ui}));
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    bar->sync();
    a.root->frame(false);
    CHECK(a.root->find("transport.recpath")->valueText().empty());
    // Golden: the transport bar as it reads with the engine running.
    const gui::Image image = a.root->readBack();
    const auto path = std::filesystem::path(SUPPLE_GUI_GOLDEN_DIR) / "transport-bar.png";
    if (std::getenv("SUPPLE_UPDATE_GOLDEN") || !std::filesystem::exists(path)) {
        REQUIRE(gui::writePng(path, image));
        MESSAGE("golden written: " << path.string());
        return;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    REQUIRE(data);
    REQUIRE(w == image.width);
    REQUIRE(h == image.height);
    std::size_t off = 0;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        int d = 0;
        for (int c = 0; c < 3; ++c)
            d = std::max(d, std::abs(int(image.rgba[i + c]) - int(data[i + c])));
        off += d > 48;
    }
    stbi_image_free(data);
    CHECK(off < image.rgba.size() / 4 / 200);
    if (auto stopped = a.dispatcher->execute({"engine.stop", json::Value::object(), ui}); !stopped)
        MESSAGE("engine.stop: " << stopped.error().message);
}

// A control's rect as it is laid out (the toolkit arranges a container's
// children in the container's own space, so this is the point to press).
void pressAt(ui::Root& root, const ui::Node& node) {
    const ui::Rect b = node.bounds();
    const ui::Point at{b.x + b.w / 2, b.y + b.h / 2};
    root.pointer({ui::PointerEvent::Type::Move, at});
    root.pointer({ui::PointerEvent::Type::Down, at, 1});
    root.pointer({ui::PointerEvent::Type::Up, at, 1});
}

TEST_CASE("the TCP's input menu lists the device's ports and picks one (P5.13)") {
    App a;
    if (!a.ok) {
        MESSAGE("skipped: no offscreen GL in this environment");
        return;
    }
    // What engine.inputs answers: three capture ports and two MIDI devices,
    // named the way PipeWire's JACK reports them; empty while stopped.
    bool running = true;
    auto inputs = [&running] {
        json::Value v = json::Value::object();
        v["running"] = running;
        v["backend"] = "stub";
        json::Value audio = json::Value::array();
        json::Value midi = json::Value::array();
        if (running) {
            for (int ch = 1; ch <= 3; ++ch) {
                json::Value e = json::Value::object();
                e["channel"] = static_cast<std::int64_t>(ch);
                e["port"] = "M Series Pro:capture_AUX" + std::to_string(ch - 1);
                audio.push_back(std::move(e));
            }
            midi.push_back(json::Value("Keystation"));
            midi.push_back(json::Value("Launchpad"));
        }
        v["audio"] = std::move(audio);
        v["midi"] = std::move(midi);
        return v;
    };
    peaks::PeakService peaks(peaks::defaultCacheDir(gui::test::slug()),
                             [](std::function<void()> f) { f(); });
    gui::MeterFeed meters;
    gui::Shell shell(*a.app, *a.dispatcher, a.ui, "", peaks, meters, inputs);
    a.root->setContent(shell.build(*a.root, *a.actions));
    const cmd::Origin ui{cmd::Origin::Type::Ui, "gui-test", {}};
    json::Value add = json::Value::object();
    add["name"] = "Gtr";
    auto created = a.dispatcher->execute({"track.add", std::move(add), ui});
    REQUIRE(created);
    const std::string track = (*created)["track"].asString();
    const model::Id id = *model::Id::parse(track);
    shell.sync();
    a.root->frame(false);
    // The TCP pools its rows, so the control is re-fetched after every change.
    auto inputButton = [&]() -> ui::Node* {
        return shell.arrange()->tcpPanel().controlNode(id, "input");
    };
    // The menu lives in the popup on the overlay: take the list out of it.
    auto openMenu = [&]() -> ui::List* {
        ui::Node* button = inputButton();
        REQUIRE(button);
        pressAt(*a.root, *button);
        a.root->frame(false);
        ui::Node* frame = a.root->find("popup");
        if (!frame) return nullptr;
        for (const auto& child : frame->children())
            if (auto* list = dynamic_cast<ui::List*>(child.get())) return list;
        return nullptr;
    };
    REQUIRE(inputButton());
    CHECK(inputButton()->valueText() == "in \u2014");  // nothing chosen yet
    // The strip shows the port names; a narrow strip clips the tail, so the
    // name is a prefix (the menu shows them whole).
    ui::List* menu = openMenu();
    REQUIRE(menu);
    CHECK(menu->count() == 9);
    menu->onActivate(1);                      // "in 2" -> record_input [2]
    CHECK(a.root->find("popup") == nullptr);  // it closed
    shell.sync();
    a.root->frame(false);
    REQUIRE(inputButton());
    CHECK(inputButton()->valueText().rfind("AUX1", 0) == 0);
    // A stereo pair is one entry ([n, n+1]).
    menu = openMenu();
    REQUIRE(menu);
    menu->onActivate(3);  // stereo 1-2
    shell.sync();
    a.root->frame(false);
    REQUIRE(inputButton());
    CHECK(inputButton()->valueText().rfind("AUX0", 0) == 0);
    // A MIDI device: the strip shows the device, and the engine clears the
    // audio channels for it.
    menu = openMenu();
    REQUIRE(menu);
    menu->onActivate(7);  // the first device (Keystation)
    shell.sync();
    a.root->frame(false);
    REQUIRE(inputButton());
    CHECK(inputButton()->valueText().rfind("Keystat", 0) == 0);
    const model::Track* tr = a.app->head().tracks.find(id);
    REQUIRE(tr);
    REQUIRE(tr->record.input);
    const model::Route* route = a.app->head().routes.find(*tr->record.input);
    REQUIRE(route);
    CHECK(route->from.midiDevice.value_or("") == "Keystation");
    CHECK_FALSE(route->from.hwChannels.has_value());
    // While the engine is stopped the device reports nothing: one disabled
    // entry, and picking it changes nothing.
    running = false;
    menu = openMenu();
    REQUIRE(menu);
    CHECK(menu->count() == 1);
    menu->onActivate(0);
    shell.sync();
    a.root->frame(false);
    REQUIRE(inputButton());
    CHECK(inputButton()->valueText().rfind("Keystat", 0) == 0);  // unchanged
    // Detach the shell's tree while the shell (and the meter feed it holds a
    // reference to) is still alive: its rows release their meters on the way
    // out.
    a.root->closePopups();
    a.root->setContent(std::make_unique<ui::Node>("detached"));
}

// Diagnostic tests asserting the observed buggy behavior; not proposed regressions.
TEST_CASE("audit3: same-version project replacement resnapshots the arrange") {
    App a;
    REQUIRE(a.ok);
    auto add = [&](const char* name) {
        json::Value args = json::Value::object();
        args["name"] = name;
        auto r = a.dispatcher->execute({"track.add", args, {}});
        REQUIRE(r);
        return *model::Id::parse((*r)["track"].asString());
    };
    const auto id = add("Old project");
    REQUIRE(a.app->save(a.dir / "old.json"));
    REQUIRE(a.app->newProject({}));
    REQUIRE(add("New project") == id);
    REQUIRE(a.app->save(a.dir / "new.json"));
    REQUIRE(a.app->open(a.dir / "old.json"));
    peaks::PeakService peaks(a.dir / "peaks", [](auto) {});
    gui::MeterFeed meters;
    gui::Shell shell(*a.app, *a.dispatcher, a.ui, "", peaks, meters);
    a.root->setContent(shell.build(*a.root, *a.actions));
    shell.sync();
    const auto shown = shell.arrange()->session().version;
    REQUIRE(a.app->open(a.dir / "new.json"));
    REQUIRE(a.app->head().version == shown);
    shell.sync();
    CHECK(a.app->head().tracks.find(id)->name == "New project");
    CHECK(shell.arrange()->session().tracks.find(id)->name == "New project");
}

TEST_CASE("audit3: missing sidecar restores defaults") {
    App a;
    REQUIRE(a.ok);
    const auto first = a.dir / "first.json", second = a.dir / "second.json";
    int value = 1;
    gui::UiPersistence persistence;
    persistence.add(
        "value", [&] { return json::Value(value); },
        [&](const auto& v) { value = static_cast<int>(v.asInt()); });
    persistence.tick(first);
    value = 123;
    REQUIRE(persistence.flush(first));
    persistence.tick(second);
    CHECK(value == 1);
    REQUIRE(persistence.flush(second));
    auto bytes = readFile(gui::UiPersistence::fileFor(second));
    REQUIRE(bytes);
    const auto doc = json::parse(std::string(bytes->begin(), bytes->end()));
    REQUIRE(doc);
    CHECK(doc->find("value")->asInt() == 1);
}

TEST_CASE("audit3: project replacement clears the item clipboard") {
    App a;
    REQUIRE(a.ok);
    auto run = [&](const char* command, json::Value args) {
        auto r = a.dispatcher->execute({command, std::move(args), {}});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    auto make = [&](const char* name) {
        auto t = run("track.add", json::Value::object());
        json::Value item = json::Value::object();
        item["track"] = t["track"];
        item["notation"] = "voice lead\n1: C4:q r:*\n";
        item["name"] = name;
        auto i = run("item.add", std::move(item));
        return std::pair{*model::Id::parse(t["track"].asString()),
                         *model::Id::parse(i["item"].asString())};
    };
    const auto [track, item] = make("OLD CLIPBOARD");
    peaks::PeakService peaks(a.dir / "peaks", [](auto) {});
    gui::MeterFeed meters;
    gui::Shell shell(*a.app, *a.dispatcher, a.ui, "", peaks, meters);
    a.root->setContent(shell.build(*a.root, *a.actions));
    shell.sync();
    auto& gestures = shell.arrange()->gestures();
    gestures.pinVersion = [&](auto version) { return a.app->versions().pin(version); };
    gestures.run = [&](const auto&, const auto& steps) {
        return a.dispatcher->execute({steps.back().command, steps.back().args, {}});
    };
    gestures.setSelection({item});
    gestures.copySelection();
    REQUIRE(a.app->newProject({}));
    CHECK_FALSE(gestures.canPaste());
    REQUIRE(gestures.paste(8, track));
    CHECK(a.app->head().items.empty());
    CHECK(make("NEW PROJECT ITEM") == std::pair{track, item});
    json::Value edit = json::Value::object();
    edit["track"] = track.str();
    edit["mute"] = true;
    run("track.set", edit);  // advance the replacement so view refreshes
    shell.sync();
    CHECK_FALSE(gestures.canPaste());
    const auto version = a.app->head().version;
    REQUIRE(gestures.paste(8, track));
    CHECK(a.app->head().version == version);
}

TEST_CASE("audit3: popup close cannot restore focus into a hidden dock") {
    App a;
    REQUIRE(a.ok);
    gui::AgentPanel panel(
        *a.root, *a.app, a.registry, *a.dispatcher, [](auto) {}, a.dir / "config" / "agent.json",
        SUPPLE_SKILL_DIR);
    auto frame = std::make_unique<ui::Node>("window");
    frame->add(panel.build());
    a.root->setContent(std::move(frame));
    panel.setVisible(true);
    a.root->frame(false);
    panel.openSettings();
    panel.setVisible(false);
    a.root->closePopup();
    CHECK(a.root->focused() == nullptr);
}

TEST_CASE("audit3: screenset import trims names and keeps the first collision") {
    json::Value applied;
    gui::Screensets sets([] { return json::Value::object(); }, [&](const auto& v) { applied = v; });
    sets.fromJson(
        json::parse(R"({" mixing ":{"first":true},"mixing":{"first":false},"   ":{}})").value());
    CHECK(sets.names() == std::vector<std::string>{"mixing"});
    REQUIRE(sets.load(" mixing "));
    CHECK(applied["first"].asBool());
    REQUIRE(sets.remove("mixing"));
}
TEST_CASE("audit3: history notices branches that share the current head") {
    App a;
    REQUIRE(a.ok);
    gui::HistoryPanel panel(*a.root, *a.actions, *a.dispatcher, *a.app);
    a.root->setContent(panel.build());
    panel.setVisible(true);
    panel.sync();
    auto args = json::Value::object();
    args["name"] = "same-head";
    args["switch"] = true;
    REQUIRE(a.dispatcher->execute({"branch.create", args, {}}));
    panel.sync();
    CHECK(panel.branches() == std::vector<std::string>{"main", "same-head"});
    CHECK(a.root->find("history.branch")->valueText() == "Branch: same-head");
    args = json::Value::object();
    args["name"] = "main";
    REQUIRE(a.dispatcher->execute({"branch.switch", args, {}}));
    args["name"] = "same-head";
    REQUIRE(a.dispatcher->execute({"branch.delete", args, {}}));
    panel.sync();
    CHECK(panel.branches() == std::vector<std::string>{"main"});
}

TEST_CASE(
    "audit3: sidecars flush before replacement, reset missing sections and preserve Save As") {
    App a;
    REQUIRE(a.ok);
    const auto one = a.dir / "one.json", two = a.dir / "two.json", copy = a.dir / "copy.json";
    int zoom = 40, panel = 0;
    gui::UiPersistence p;
    p.add(
        "zoom", [&] { return json::Value(zoom); },
        [&](const auto& v) { zoom = static_cast<int>(v.asInt()); });
    p.add(
        "panel", [&] { return json::Value(panel); },
        [&](const auto& v) { panel = static_cast<int>(v.asInt()); });
    p.tick(one, 1);
    zoom = 123;
    panel = 1;
    p.prepareProjectSwitch();  // still the old UI, before file-action callbacks clear it
    zoom = 99;
    panel = 9;
    REQUIRE(writeFileAtomic(gui::UiPersistence::fileFor(two),
                            R"({"schema":"supple.ui/1.0","zoom":80})"));
    p.tick(two, 2);
    CHECK(zoom == 80);
    CHECK(panel == 0);
    auto bytes = readFile(gui::UiPersistence::fileFor(one));
    REQUIRE(bytes);
    auto old = json::parse(std::string(bytes->begin(), bytes->end()));
    REQUIRE(old);
    CHECK((*old)["zoom"].asInt() == 123);
    CHECK((*old)["panel"].asInt() == 1);
    zoom = 160;
    panel = 1;
    REQUIRE(writeFileAtomic(gui::UiPersistence::fileFor(copy),
                            R"({"schema":"supple.ui/1.0","zoom":12,"panel":0})"));
    p.tick(copy, 2);
    CHECK(zoom == 160);
    CHECK(panel == 1);
    REQUIRE(p.flush(copy));
    p.tick(one, 3);
    CHECK(zoom == 123);
    CHECK(panel == 1);
    // Reopening the same pathname is also a replacement.
    REQUIRE(writeFileAtomic(gui::UiPersistence::fileFor(one),
                            R"({"schema":"supple.ui/1.0","zoom":65})"));
    p.prepareProjectSwitch();  // updates the old sidecar first
    REQUIRE(writeFileAtomic(gui::UiPersistence::fileFor(one),
                            R"({"schema":"supple.ui/1.0","zoom":65})"));
    p.tick(one, 4);
    CHECK(zoom == 65);
    CHECK(panel == 0);
}
TEST_CASE("audit3: unsupported sidecar major resets defaults and stays byte-identical") {
    App a;
    REQUIRE(a.ok);
    const auto project = a.dir / "future.json";
    const std::string future = R"({"schema":"supple.ui/2.0","zoom":777})";
    REQUIRE(writeFileAtomic(gui::UiPersistence::fileFor(project), future));
    int zoom = 40;
    gui::UiPersistence p;
    p.add(
        "zoom", [&] { return json::Value(zoom); },
        [&](const auto& v) { zoom = static_cast<int>(v.asInt()); });
    zoom = 123;
    p.tick(project, 1);
    CHECK(zoom == 40);
    zoom = 160;
    REQUIRE(p.flush(project));
    p.prepareProjectSwitch();
    p.tick(a.dir / "next.json", 2);
    auto bytes = readFile(gui::UiPersistence::fileFor(project));
    REQUIRE(bytes);
    CHECK(std::string(bytes->begin(), bytes->end()) == future);
}

TEST_CASE("mixer and MIDI editor key bindings run when their section is active") {
    App a;
    REQUIRE(a.ok);
    a.actions->keymap().bind({{*actions::parseChord("Ctrl+Alt+Q")}, "mixer", "track.add:channel"});
    const auto before = a.tracks("channel");
    CHECK_FALSE(a.actions->key(SDLK_Q, ui::kCtrl | ui::kAlt, false));  // arrange, global
    CHECK(a.tracks("channel") == before);
    CHECK(a.actions->key(SDLK_Q, ui::kCtrl | ui::kAlt, false, {"mixer", "arrange", "global"}));
    CHECK(a.tracks("channel") == before + 1);
}

TEST_CASE("toolbar, menu and key actions that need typed arguments ask for them") {
    App a;
    REQUIRE(a.ok);
    std::string seen;
    a.actions->onPrompt = [&](const actions::Action& action) {
        gui::openActionPrompt(*a.root, *a.actions, action);
    };
    a.actions->addUi("probe.prompt", "Prompt", "name", [&](const json::Value& v) -> Status {
        seen = v.isString() ? v.asString() : "?";
        return {};
    });
    gui::ChromeLayout layout;
    layout.toolbar = {"probe.prompt"};
    auto frame = std::make_unique<ui::Node>("window");
    frame->add(gui::makeChrome(*a.root, *a.actions, layout));
    a.root->setContent(std::move(frame));
    a.root->frame(false);
    auto* button = dynamic_cast<ui::Button*>(a.root->find("tool.probe.prompt"));
    REQUIRE(button);
    button->onClick();
    REQUIRE(a.root->popupOpen());  // the form, as in the actions list
    auto* field = dynamic_cast<ui::TextField*>(a.root->find("actions.prompt.name"));
    REQUIRE(field);
    field->setText("Lead");
    REQUIRE(field->onSubmit);
    field->onSubmit("Lead");
    CHECK(seen == "Lead");
}

TEST_CASE("a macro passes its typed arguments to the steps that ask for them") {
    App a;
    REQUIRE(a.ok);
    std::string seen;
    a.actions->addUi("probe.typed", "Typed step", "name", [&](const json::Value& v) -> Status {
        seen = v.isString() ? v.asString() : "?";
        return {};
    });
    actions::Action macro;
    macro.id = "macro:typed";
    macro.name = "Typed macro";
    macro.prompts = {"name"};
    macro.steps = {"probe.typed"};
    REQUIRE(a.actions->run(macro, json::parse(R"({"name":"provided"})").value()));
    CHECK(seen == "provided");
    actions::Action loop;  // a macro that contains itself stops instead of recursing
    loop.id = "macro:loop";
    loop.name = "Loop";
    loop.steps = {"macro:loop"};
    a.actions->registry().add(loop);
    auto r = a.actions->run(*a.actions->registry().find("macro:loop"));
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "action.macro_depth");
}
