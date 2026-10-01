// The FX chain window, the plugin browser and the parameter panel, and the
// render dialog: driven by keys and clicks through the bus (gui.md §10).
#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <stb_image.h>

#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "app_actions.h"
#include "fx_window.h"
#include "generate.h"
#include "plugin_browser.h"
#include "render_dialog.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/host/clap_host.h"
#include "supple/ui/list.h"
#include "supple/ui/root.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

using namespace supple;

namespace {

const cmd::Origin kTestOrigin{cmd::Origin::Type::Test, "fx-render-test", {}};

struct Fixture {
    std::filesystem::path dir, configDir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    gui::UiState ui;
    std::unique_ptr<ui::Root> root;
    std::unique_ptr<gui::AppActions> actions;
    std::unique_ptr<gui::FxWindow> fx;
    std::unique_ptr<gui::RenderDialog> render;
    bool ok = false;

    Fixture() {
        dir = std::filesystem::path(__FILE__)
                  .parent_path()
                  .parent_path()
                  .parent_path()
                  .parent_path() /
              "build" / ("supple-fx-render-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        configDir = dir / "config";
        std::filesystem::create_directories(configDir);
        ::setenv("XDG_CONFIG_HOME", configDir.c_str(), 1);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"fx-render-test", 1100, 740, true});
        if (!w) return;
        window = std::move(*w);
        auto f = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        if (!f) return;
        fonts = *f;
        engine::AppOptions options;
        options.workdir = dir;
        options.chainDir = dir / "chains";
        options.pluginPath = {std::filesystem::path(SUPPLE_CLAP_DIR)};
        options.identity = host::HostIdentity{"Quire test", "Supple Audio", "", "0"};
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
        fx = std::make_unique<gui::FxWindow>(*app, *dispatcher, *root, gui::FxWindow::Hooks{},
                                             gui::Favourites::defaultFile("quire-test"));
        render = std::make_unique<gui::RenderDialog>(*app, *dispatcher, *root);
        actions->addUi("ui.fx.chain", "FX: chain…", "track",
                       [this](const json::Value& v) -> Status { return fx->openFor(ui, v); });
        actions->addUi("ui.render", "File: render…", [this] { render->open(); });
        root->onUnhandledKey = [this](const ui::KeyEvent& e) {
            const bool typing = root->focused() && root->focused()->role() == ui::Role::TextField;
            return actions->key(e.key, e.mods, typing);
        };
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Row;
        frame->emplace<ui::Node>("body");
        root->setContent(std::move(frame));
        root->frame(false);
        ok = true;
    }
    ~Fixture() {
        // Destroy the panels before the root they point into.
        render.reset();
        fx.reset();
        actions.reset();
        root.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }

    Result<json::Value> run(const char* command, json::Value args) {
        return dispatcher->execute({command, std::move(args), kTestOrigin});
    }
    json::Value args(std::initializer_list<std::pair<const char*, json::Value>> fields) {
        json::Value o = json::Value::object();
        for (const auto& [k, v] : fields) o[k] = v;
        return o;
    }
    model::Id addTrack(const char* name) {
        auto r = run("track.add", args({{"type", "channel"}, {"name", name}}));
        REQUIRE(r);
        auto id = model::Id::parse((*r)["track"].asString());
        REQUIRE(id);
        return *id;
    }
    // The first channel strip (generateSession makes tr_1, tr_2, …).
    model::Id firstChannel() {
        for (const auto& e : app->head().tracks)
            if (e.value->type == model::StripType::Channel) return e.id;
        FAIL("no channel track");
        return {};
    }
    std::size_t trackCount() { return app->head().tracks.size(); }
    // The media path of the first item on the track named `name`.
    std::string sourcePathOf(const std::string& name) {
        const model::Session& s = app->head();
        for (const auto& t : s.tracks)
            if (t.value->name == name)
                for (const auto& i : s.items)
                    if (i.value->track == t.id && i.value->activeTake) {
                        const model::Take* take = s.takes.find(*i.value->activeTake);
                        if (!take || !take->source) continue;
                        if (const model::MediaSource* media = s.media.find(*take->source))
                            return media->path;
                    }
        return {};
    }
    void frame() {
        root->damageAll();
        root->frame(false);
    }
    // Clicks need a layout pass: a popup has no bounds until the next frame.
    bool click(const std::string& id) {
        frame();
        const bool hit = root->click(id);
        frame();
        return hit;
    }
    void type(std::string_view text) {
        root->text(text);
        frame();
    }
    void key(const char* chord) {
        const auto c = actions::parseChord(chord);
        REQUIRE(c);
        root->key(c->key, c->mods);
        frame();
    }
    std::size_t historyCount() {
        // The full linear undo depth (history.list caps at 20 entries).
        return app->versions().history(1000).size();
    }
    std::string fxStatus() {
        auto* label = dynamic_cast<ui::Label*>(root->find("fx.status"));
        return label ? label->text() : std::string();
    }
    void snapshot(const char* name) {
        const char* out = std::getenv("SUPPLE_TEST_SNAPSHOTS");
        if (!out) return;
        frame();
        (void)gui::writePng(std::filesystem::path(out) / name, root->readBack());
    }
    void golden(const char* name) {
        const gui::Image image = root->readBack();
        const auto path = std::filesystem::path(SUPPLE_GUI_GOLDEN_DIR) / name;
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
    }
    // The stock plugins build with ALL: a partial build has none, and the FX
    // tests skip rather than fail.
    bool stockPlugins() {
        auto r = run("plugin.list", json::Value::object());
        return r && !(*r)["plugins"].asArray().empty();
    }
    // Finds a stock plugin id by display name (the scan ordering varies).
    std::string pluginId(const char* name) {
        auto r = run("plugin.list", json::Value::object());
        REQUIRE(r);
        for (const json::Value& p : (*r)["plugins"].asArray()) {
            const json::Value* n = p.find("name");
            if (n && n->isString() && n->asString() == name) return p.find("id")->asString();
        }
        FAIL("no plugin named " << name);
        return {};
    }
};

}  // namespace

TEST_CASE("the FX chain: browser, bypass/offline, reorder, remove, parameters, favourites") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    const model::Id track = f.addTrack("Guitar");
    const std::string plumb = f.pluginId("Plumb");
    const std::string facet = f.pluginId("Facet");

    REQUIRE(f.fx->open(track));
    REQUIRE(f.fx->isOpen());
    CHECK(f.fx->chain().empty());
    // Add a stock plugin from the browser.
    f.click("fx.add");
    REQUIRE(f.fx->browser().isOpen());
    auto* search = dynamic_cast<ui::TextField*>(f.root->find("browser.search"));
    REQUIRE(search);
    f.type("Plumb");
    f.key("Return");
    CHECK_FALSE(f.fx->browser().isOpen());
    REQUIRE(f.fx->chain().size() == 1);
    const model::Id fx1 = f.fx->chain()[0];
    CHECK(f.app->head().fx.find(fx1)->plugin.name == "Plumb");
    CHECK(f.app->head().fx.find(fx1)->plugin.id == plumb);

    // Add a second one and reorder with the keyboard-free ▲/▼ buttons (fx.move).
    f.click("fx.add");
    f.type("Facet");
    f.key("Return");
    REQUIRE(f.fx->chain().size() == 2);
    const model::Id fx2 = f.fx->chain()[1];
    CHECK(f.app->head().fx.find(fx2)->plugin.id == facet);
    f.click("fx.up." + fx2.str());
    REQUIRE(f.fx->chain().size() == 2);
    CHECK(f.fx->chain()[0] == fx2);
    CHECK(f.fx->chain()[1] == fx1);
    f.click("fx.down." + fx2.str());
    CHECK(f.fx->chain()[0] == fx1);

    // Reorder by keyboard too (Alt+Down / Alt+Up on a focused row).
    f.root->focus(f.root->find("fx.row." + fx1.str()));
    f.key("Alt+Down");
    CHECK(f.fx->chain()[0] == fx2);
    f.key("Alt+Up");
    CHECK(f.fx->chain()[0] == fx1);

    // Bypass and offline are distinct fields (fx.set).
    f.click("fx.bypass." + fx1.str());
    CHECK(f.app->head().fx.find(fx1)->bypass);
    CHECK_FALSE(f.app->head().fx.find(fx1)->offline);
    f.click("fx.offline." + fx1.str());
    CHECK(f.app->head().fx.find(fx1)->bypass);
    CHECK(f.app->head().fx.find(fx1)->offline);
    CHECK(f.app->head().fx.find(fx2)->bypass == false);

    // A wet/dry drag is one undo step too (the cap sits at 100 %: drag left).
    ui::Node* wet = f.root->find("fx.wet." + fx1.str());
    REQUIRE(wet);
    const std::size_t wetBefore = f.historyCount();
    const float wetY = wet->bounds().y + wet->bounds().h / 2;
    const float wetX = wet->bounds().x + wet->bounds().w - 12;
    f.root->pointer({ui::PointerEvent::Type::Down, {wetX, wetY}, 1});
    for (int i = 1; i <= 20; ++i)
        f.root->pointer({ui::PointerEvent::Type::Move, {wetX - i * 6.0f, wetY}, 1});
    f.root->pointer({ui::PointerEvent::Type::Up, {wetX - 140.0f, wetY}, 1});
    CHECK(f.historyCount() == wetBefore + 1);
    CHECK(f.app->head().fx.find(fx1)->wet < 100.0);

    // The semantic parameter panel: a value set with units.
    f.click("fx.params." + fx1.str());
    auto* gainText = dynamic_cast<ui::TextField*>(f.root->find("fx.param.text.gain"));
    REQUIRE(gainText);
    f.root->focus(gainText);
    f.key("Ctrl+A");  // the field holds the formatted value: replace it
    f.type("-6 dB");
    f.key("Return");
    auto gain = f.run("fx.param.get", f.args({{"fx", fx1.str()}, {"param", "gain"}}));
    REQUIRE(gain);
    CHECK((*gain)["value"].asDouble() == doctest::Approx(-6.0));

    // A 20-move slider drag is one undo step (Origin::gesture).
    ui::Node* slider = f.root->find("fx.param.gain");
    REQUIRE(slider);
    const std::size_t before = f.historyCount();
    const float y = slider->bounds().y + slider->bounds().h / 2;
    const float x0 = slider->bounds().x + 12;
    f.root->pointer({ui::PointerEvent::Type::Down, {x0, y}, 1});
    for (int i = 1; i <= 20; ++i)
        f.root->pointer({ui::PointerEvent::Type::Move, {x0 + i * 8.0f, y}, 1});
    f.root->pointer(
        {ui::PointerEvent::Type::Up, {slider->bounds().x + slider->bounds().w - 12, y}, 1});
    CHECK(f.historyCount() == before + 1);
    auto moved = f.run("fx.param.get", f.args({{"fx", fx1.str()}, {"param", "gain"}}));
    REQUIRE(moved);
    CHECK((*moved)["value"].asDouble() > -6.0);

    // An out-of-range value is rejected by the plugin and the reason shown.
    f.root->focus(gainText);
    f.key("Ctrl+A");
    f.type("9999 dB");
    f.key("Return");
    CHECK_FALSE(f.fxStatus().empty());

    // Favourites persist to the user config.
    f.click("fx.add");
    f.type("Plumb");
    f.click("browser.fav");
    const auto favFile = gui::Favourites::defaultFile("quire-test");
    CHECK(std::filesystem::exists(favFile));
    auto loaded = gui::Favourites::load(favFile);
    REQUIRE(loaded);
    CHECK(loaded->has(plumb));
    CHECK(f.fx->browser().favourites().has(plumb));
    f.key("Escape");  // close the browser
    CHECK_FALSE(f.fx->browser().isOpen());

    // The input chain: add one from here (fx.add {chain:"input"}), and its
    // rows use the same controls (ifx. ids).
    f.click("fx.tab.chain");
    f.click("fx.add.input");
    REQUIRE(f.fx->browser().isOpen());
    f.type("Plumb");
    f.key("Return");
    CHECK(f.fx->inputChain().size() == 1);
    CHECK(f.fx->chain().size() == 2);  // the main chain is untouched
    const model::Id inputFx = f.fx->inputChain()[0];
    CHECK(f.app->head().fx.find(inputFx)->plugin.name == "Plumb");
    CHECK(f.root->find("ifx.row." + inputFx.str()) != nullptr);
    f.click("ifx.remove." + inputFx.str());
    CHECK(f.fx->inputChain().empty());

    // Remove (from the chain tab: the rows live there).
    f.click("fx.tab.chain");
    f.click("fx.remove." + fx1.str());
    REQUIRE(f.fx->chain().size() == 1);
    CHECK(f.fx->chain()[0] == fx2);
}

TEST_CASE("golden images: the FX chain, the plugin browser and the render dialog") {
    Fixture f;
    if (!f.ok) return;
    if (!f.stockPlugins()) {
        MESSAGE("skipped: no stock plugins (a partial build)");
        return;
    }
    REQUIRE(gui::generateSession(*f.dispatcher, f.app->mediaDir(), 2));
    const model::Id track = f.firstChannel();

    REQUIRE(f.fx->open(track));
    f.click("fx.add");
    f.type("Plumb");
    f.key("Return");
    f.click("fx.add");
    f.type("Facet");
    f.key("Return");
    f.click("fx.offline." + f.fx->chain()[0].str());
    f.frame();
    f.snapshot("fx-chain.png");
    f.golden("fx-chain-dark.png");

    f.click("fx.add");
    f.type("Plumb");
    f.click("browser.fav");  // mark the match a favourite (it moves to the top)
    f.root->focus(dynamic_cast<ui::TextField*>(f.root->find("browser.search")));
    f.key("Ctrl+A");
    f.key("Delete");
    f.frame();
    f.snapshot("plugin-browser.png");
    f.golden("plugin-browser-dark.png");
    f.key("Escape");

    f.click("fx.tab.params");
    f.frame();
    f.snapshot("fx-params.png");

    f.fx->close();  // a clean render-dialog golden
    f.render->open();
    f.render->setField("render.pattern", "{project}-{track_number:02}-{track|lower}");
    f.render->refreshPreview();
    f.frame();
    f.snapshot("render-dialog.png");
    f.golden("render-dialog-dark.png");
}

TEST_CASE("the plugin browser names the plugin's file and marks a shadowed copy") {
    Fixture f;
    if (!f.ok) return;
    const model::Id track = f.addTrack("Guitar");
    REQUIRE(f.fx->open(track));
    f.click("fx.add");
    REQUIRE(f.fx->browser().isOpen());
    f.type("Plumb");
    // The detail line names the file the plugin would be loaded from.
    CHECK(f.fx->browser().detail().find("Plumb.clap") != std::string::npos);
    CHECK(f.fx->browser().detail().find("shadowed") == std::string::npos);
    CHECK(f.fx->browser().rowText(0).vendor == "stock");
    f.key("Escape");

    // A copy of the same plugin earlier on the search path shadows the built
    // one; the browser says which copy wins and which is ignored.
    const std::filesystem::path dup = f.dir / "shadow";
    std::filesystem::create_directories(dup);
    std::filesystem::copy_file(std::filesystem::path(SUPPLE_CLAP_DIR) / "Plumb.clap",
                               dup / "Plumb.clap",
                               std::filesystem::copy_options::overwrite_existing);
    engine::AppOptions options;
    options.workdir = f.dir / "shadow-work";
    options.pluginPath = {dup, std::filesystem::path(SUPPLE_CLAP_DIR)};
    options.identity = host::HostIdentity{"Quire test", "Supple Audio", "", "0"};
    auto app = engine::EngineApp::create(options);
    REQUIRE(app);
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, **app);
    (*app)->attachCommands(bus);
    gui::PluginBrowser browser(**app, bus, *f.root, f.configDir / "quire-test" / "favourites.json");
    browser.open([](const std::string&) {});
    auto* search = dynamic_cast<ui::TextField*>(f.root->find("browser.search"));
    REQUIRE(search);
    f.root->focus(search);
    f.type("Plumb");
    auto* list = dynamic_cast<ui::List*>(f.root->find("browser.results"));
    REQUIRE(list);
    bool sawShadowed = false, sawWinner = false;
    std::optional<std::size_t> shadowedRow;
    for (std::size_t i = 0; i < browser.rowCount(); ++i) {
        const auto row = browser.rowText(i);
        if (row.id != "com.suppleaudio.utility") continue;
        if (row.vendor == "shadowed") {
            sawShadowed = true;
            shadowedRow = i;
        } else {
            sawWinner = true;
        }
    }
    CHECK(sawWinner);
    CHECK(sawShadowed);
    REQUIRE(shadowedRow);
    list->select(*shadowedRow, true);  // as a click or the arrow keys would
    // The shadowed copy is the built one: its own file, and why it is ignored.
    CHECK(browser.detail().find("Plumb.clap") != std::string::npos);
    CHECK(browser.detail().find("shadowed") != std::string::npos);
    // The copy the loader would use is the one earlier on the search path.
    list->select(*shadowedRow == 0 ? 1 : 0, true);
    CHECK(browser.detail().find(dup.string()) != std::string::npos);
    browser.close();
}

TEST_CASE("the FX chain badges each effect's state, and a failed one offers a restart") {
    Fixture f;
    if (!f.ok) return;
    const model::Id track = f.addTrack("Guitar");
    REQUIRE(f.fx->open(track));
    f.click("fx.add");
    f.type("Plumb");
    f.key("Return");
    REQUIRE(f.fx->chain().size() == 1);
    const model::Id fx = f.fx->chain()[0];
    CHECK(f.root->find("fx.badge." + fx.str())->valueText() == "in-process");
    f.click("fx.offline." + fx.str());
    CHECK(f.root->find("fx.badge." + fx.str())->valueText() == "in-process · offline");
    f.click("fx.offline." + fx.str());  // back on
    CHECK(f.root->find("fx.badge." + fx.str())->valueText() == "in-process");
    // The isolation modes and a failed instance compose the same badge.
    model::FxInstance inst;
    inst.isolation = model::Isolation::Dedicated;
    CHECK(gui::FxWindow::badgeText(inst, false) == "isolated");
    inst.isolation = model::Isolation::SharedBridge;
    inst.offline = true;
    CHECK(gui::FxWindow::badgeText(inst, true) == "shared bridge · offline · failed");
    // A healthy engine reports no failure: no Restart button, and tick() keeps
    // the set empty.
    f.fx->tick();
    CHECK(f.fx->failed().empty());
    CHECK(f.root->find("fx.restart." + fx.str()) == nullptr);
}

TEST_CASE("the render dialog: the preview matches render.dry-run and a job lists its file") {
    Fixture f;
    if (!f.ok) return;
    REQUIRE(gui::generateSession(*f.dispatcher, f.app->mediaDir(), 1));

    f.render->open();
    f.render->setField("render.end", "8");  // a short render: fast and load-insensitive
    // An empty field explains the default the engine would use.
    const auto placeholder = [&](const char* id) {
        auto* field = dynamic_cast<ui::TextField*>(f.root->find(id));
        REQUIRE(field);
        return field->placeholder();
    };
    CHECK(placeholder("render.end").find("last item end") == 0);
    CHECK(placeholder("render.sample_rate") == "48000");
    CHECK(placeholder("render.bit_depth") == "32");
    CHECK(placeholder("render.channels") == "2");
    CHECK(placeholder("render.tail") == "0");
    CHECK(placeholder("render.normalize") == "none");
    f.render->setField("render.pattern", "preview-{counter:02}");
    f.render->refreshPreview();

    // The preview matches render.dry-run for the same arguments.
    auto dry = f.run(
        "render.dry-run",
        f.args({{"operation", "mix"}, {"pattern", "preview-{counter:02}"}, {"end_beat", 8.0}}));
    REQUIRE(dry);
    std::string expected;
    for (const json::Value& file : (*dry)["files"].asArray()) {
        const std::string path = file.find("path")->asString();
        const auto slash = path.find_last_of('/');
        expected += (expected.empty() ? "" : "\n") +
                    (slash == std::string::npos ? path : path.substr(slash + 1));
    }
    REQUIRE_FALSE(expected.empty());
    CHECK(f.render->preview().find(expected.substr(0, expected.find('\n'))) == 0);

    // Editing the pattern updates the preview.
    const std::string before = f.render->preview();
    f.render->setField("render.pattern", "other-{counter:02}");
    f.render->refreshPreview();
    CHECK(f.render->preview() != before);
    CHECK(f.render->preview().find("other-01.wav") != std::string::npos);

    // Stems preview over two tracks names each track's file.
    f.render->setSource(gui::RenderDialog::Source::Stems);
    f.render->setField("render.pattern", "stem-{track_number:02}");
    f.render->refreshPreview();
    CHECK(f.render->preview().find("stem-01.wav") != std::string::npos);

    // A render job completes and lists its file.
    f.render->setSource(gui::RenderDialog::Source::Master);
    f.render->setField("render.pattern", "job-{counter:02}");
    f.render->refreshPreview();
    f.render->run();
    REQUIRE_FALSE(f.render->job().empty());
    for (int i = 0; i < 6000 && !f.render->job().empty(); ++i) {
        f.render->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(f.render->resultFiles().size() == 1);
    CHECK(std::filesystem::exists(f.render->resultFiles()[0]));
    CHECK(f.render->statusText().find("1 file") != std::string::npos);
    // The result list carries the analysis summary when the render has one.
    CHECK(f.root->find("render.results")->valueText().find("LUFS") != std::string::npos);

    // add_to_project: the file lands as an item on a new track (one undo step).
    const std::size_t tracksBefore = f.trackCount();
    const std::size_t historyBefore = f.historyCount();
    CHECK_FALSE(f.render->addToProject());
    f.click("render.add");
    CHECK(f.render->addToProject());
    CHECK(f.root->find("render.add")->valueText() == "[x] Add to project");
    f.render->setField("render.pattern", "added-{counter:02}");
    f.render->refreshPreview();
    f.render->run();
    REQUIRE_FALSE(f.render->job().empty());
    for (int i = 0; i < 6000 && !f.render->job().empty(); ++i) {
        f.render->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    f.dispatcher->flushJobEvents();  // the follow-up runs on the model thread
    REQUIRE(f.render->resultFiles().size() == 1);
    CHECK(f.trackCount() == tracksBefore + 1);
    CHECK(f.sourcePathOf("added-01") == f.render->resultFiles()[0]);
    // The import is its own undo step; starting a job changes no model state.
    CHECK(f.historyCount() == historyBefore + 1);
}

TEST_CASE("FX chain preset buttons save, append and Shift-replace through the bus") {
    Fixture f;
    if (!f.ok || !f.stockPlugins()) {
        MESSAGE("skipped: no GUI or stock plugins");
        return;
    }
    const auto source = f.addTrack("Source");
    const auto target = f.addTrack("Target");
    REQUIRE(f.run("fx.add", f.args({{"track", source.str()},
                                    {"plugin", f.pluginId("Plumb")},
                                    {"patch", json::parse(R"({"params":{"gain":-6}})").value()}})));
    REQUIRE(f.fx->open(source));
    f.frame();
    const auto beforeSave = f.historyCount();
    REQUIRE(f.click("fx.chain.save"));
    REQUIRE(f.root->focused()->id() == "fx.chain.name");
    f.type("Guitar");
    f.key("Return");
    CHECK(f.historyCount() == beforeSave);
    CHECK(f.fxStatus() == "Saved Guitar");
    auto listed = f.run("fx.chain.list", json::Value::object());
    REQUIRE(listed);
    CHECK((*listed)["chains"].size() == 1);
    REQUIRE(f.fx->open(target));
    const auto beforeLoad = f.historyCount();
    REQUIRE(f.click("fx.chain.load"));
    REQUIRE(f.root->find("fx.chain.menu"));
    f.key("Home");
    f.key("Return");
    CHECK(f.historyCount() == beforeLoad + 1);
    REQUIRE(f.fx->chain().size() == 1);
    const auto loaded = f.fx->chain()[0];
    CHECK(f.app->head().fx.find(loaded)->plugin.id == f.pluginId("Plumb"));
    auto patch = f.run("fx.patch.get", f.args({{"fx", loaded.str()}}));
    auto original = f.run("fx.patch.get", f.args({{"fx", "fx_1"}}));
    REQUIRE(patch);
    REQUIRE(original);
    CHECK(*patch == *original);
    REQUIRE(f.click("fx.chain.load"));
    f.key("Home");
    f.key("Return");
    REQUIRE(f.fx->chain().size() == 2);
    const auto beforeReplace = f.historyCount();
    REQUIRE(f.click("fx.chain.load"));
    f.key("Home");
    f.key("Shift+Return");
    CHECK(f.historyCount() == beforeReplace + 1);
    CHECK(f.fx->chain().size() == 1);
    REQUIRE(f.run("history.undo", json::Value::object()));
    f.fx->refresh();
    CHECK(f.fx->chain().size() == 2);
    // The same controls can target the input chain without changing main FX.
    REQUIRE(f.click("fx.chain.target"));
    REQUIRE(f.click("fx.chain.load"));
    f.key("Home");
    f.key("Return");
    CHECK(f.app->head().tracks.find(target)->inputChain.size() == 1);
    CHECK(f.app->head().tracks.find(target)->chain.size() == 2);
    REQUIRE(f.click("fx.chain.save"));
    f.type("Input");
    f.key("Return");
    CHECK(f.fxStatus() == "Saved Input");
}
