// The arrange view (gui.md §4.3) over a real session built through the
// command bus (the same commands an agent uses), drawn offscreen: golden
// images for the ruler and the item states, and scripted tests for zoom,
// alignment, the playhead overlay and virtualization.
#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <stb_image.h>

#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../app/generate.h"
#include "meter_feed.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/model/session.h"
#include "supple/ui/root.h"
#include "supple/views/arrange.h"

using namespace supple;

namespace {

// One offscreen window shared by the tests (GL contexts are not free).
struct Gl {
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    bool ok = false;
    Gl() {
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"arrange-test", 1000, 560, true});
        if (!w) return;
        window = std::move(*w);
        auto f = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        if (!f) return;
        fonts = *f;
        ok = true;
    }
};

Gl& gl() {
    static Gl instance;
    return instance;
}

// A session built through the bus, with a peak service.
struct Session {
    std::filesystem::path dir;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    std::unique_ptr<peaks::PeakService> peaks;
    std::vector<std::function<void()>> posted;
    std::mutex m;

    bool build(int trackCount) {
        dir = std::filesystem::temp_directory_path() /
              ("supple-arrange-test-" + std::to_string(::getpid()) + "-" +
               std::to_string(trackCount));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        engine::AppOptions options;
        options.workdir = dir;
        auto a = engine::EngineApp::create(options);
        if (!a) return false;
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
        if (!gui::generateSession(*dispatcher, app->mediaDir(), trackCount)) return false;
        peaks =
            std::make_unique<peaks::PeakService>(dir / "cache", [this](std::function<void()> fn) {
                std::lock_guard lock(m);
                posted.push_back(std::move(fn));
            });
        return true;
    }
    ~Session() {
        peaks.reset();
        dispatcher.reset();
        app.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void drain() {
        std::vector<std::function<void()>> now;
        {
            std::lock_guard lock(m);
            now.swap(posted);
        }
        for (auto& fn : now) fn();
    }
    views::ArrangeView::Resolve resolve() const {
        return [this](const model::MediaSource& src) {
            const std::filesystem::path p(src.path);
            return p.is_absolute() ? p : app->mediaDir() / p;
        };
    }
};

Session& session6() {
    static Session s;
    static const bool once = s.build(6);
    (void)once;
    return s;
}

// Frames until the view has every visible pyramid. A deadline, not a frame
// count: under a parallel ctest the peak worker can take several seconds.
void settle(ui::Root& root, views::Lanes& lanes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        session6().drain();
        root.damageAll();
        root.frame(false);
        if (lanes.stats().pending == 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL("peaks never became ready");
}

views::ArrangeView* mount(ui::Root& root) {
    Session& s = session6();
    auto arrange =
        std::make_unique<views::ArrangeView>(*s.peaks, s.resolve(), views::TransportSource{});
    arrange->show(s.app->head());
    views::ArrangeView* view = arrange.get();
    root.setContent(std::move(arrange));
    return view;
}

// Golden images. The session is shared across the suite (and some tests edit
// it: renames, mute/solo, faders), so regenerate goldens with a FULL run
// (SUPPLE_UPDATE_GOLDEN=1, no -tc filter) — a single-test run would write them
// from a different session state.
void golden(ui::Root& root, const char* name) {
    const gui::Image image = root.readBack();
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
    CHECK(off < image.rgba.size() / 4 / 200);  // < 0.5 % of pixels
}

}  // namespace

TEST_CASE("the arrange view draws every strip's items where the engine plays them") {
    Gl& g = gl();
    if (!g.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    views::Viewport v;
    v.startSeconds = 0;
    v.pixelsPerSecond = 12;  // 768 px ≈ 64 s: the audio and MIDI items
    arrange->setViewport(v);
    settle(root, arrange->lanes());
    CHECK(arrange->lanes().stats().rows == 6);
    CHECK(arrange->lanes().stats().items == 12);  // an audio and a MIDI item per strip
    CHECK(arrange->lanes().stats().notes == 6 * 64);
    CHECK(arrange->lanes().stats().columns > 6 * 300);
    CHECK(arrange->lanes().stats().fades == 12);  // fade-in and fade-out per audio item
    CHECK(s.peaks->stats().built == 6);
    CHECK(arrange->trackLayout().rowCount() == 6);
}

TEST_CASE("rows and items outside the viewport are not drawn") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    arrange->trackLayout().setGlobalHeight(150);
    views::Viewport v;
    v.startSeconds = 40;  // the MIDI items only (bar 21 = 40 s at 120 BPM)
    v.pixelsPerSecond = 40;
    v.scrollY = 1e6f;  // clamped to the bottom: only the last rows show
    arrange->setViewport(v);
    settle(root, arrange->lanes());
    CHECK(arrange->lanes().stats().rows >= 1);
    CHECK(arrange->lanes().stats().rows < 6);  // the top rows scrolled away
    CHECK(arrange->lanes().stats().items > 0);
    CHECK(arrange->lanes().stats().notes > 0);
    CHECK(arrange->lanes().stats().columns == 0);  // the audio items end before 40 s
    CHECK(arrange->rowRect(0).bottom() <= arrange->lanesRect().y);  // row 1 is gone
}

TEST_CASE("the ruler adapts its tick density to the zoom without overlapping labels") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    auto frame = [&](double pps) {
        views::Viewport v;
        v.startSeconds = 0;
        v.pixelsPerSecond = pps;
        arrange->setViewport(v);
        root.damageAll();
        root.frame(false);
        return arrange->rulerStats();
    };
    const views::RulerStats bars = frame(4);     // out: bars only
    const views::RulerStats beats = frame(160);  // in: beats labelled
    const views::RulerStats subs = frame(400);   // closer: sub-beat ticks
    CHECK(bars.beatLabels == 0);
    CHECK(bars.subTicks == 0);
    CHECK(bars.barLabels > 2);
    CHECK(beats.beatLabels > 0);
    CHECK(subs.subTicks > beats.subTicks);
    // No two labels can touch: every gap clears the previous label's width by
    // at least the 12-unit gap the ruler enforces.
    CHECK(bars.minBarSlack >= 0.0f);
    CHECK(beats.minBarSlack >= 0.0f);
    CHECK(subs.minBarSlack >= 0.0f);
    CHECK(bars.minTimeSlack >= 0.0f);
    CHECK(beats.minTimeSlack >= 0.0f);
    CHECK(subs.minTimeSlack >= 0.0f);
    CHECK(bars.minTimeLabelGap > 30.0f);
    CHECK(subs.minTimeLabelGap > 30.0f);
}

TEST_CASE("the ruler shows the tempo and meter changes the session carries") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    const time::TimeMap& tm = *s.app->head().timeMap;
    REQUIRE(tm.tempo().size() == 2);
    CHECK(tm.tempo()[1].bpm == doctest::Approx(140.0));
    CHECK(tm.tempo()[1].beat == doctest::Approx(128.0));
    // Bar 35 (the meter change) is where the 3/4 begins.
    const auto meter = tm.meterAt(35);
    CHECK(meter.numerator == 3);
    CHECK(meter.denominator == 4);
    CHECK(tm.meterAt(34).numerator == 4);
}

TEST_CASE("zoom keeps the time under the pointer") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    views::Viewport v;
    v.startSeconds = 40;
    v.pixelsPerSecond = 40;
    arrange->setViewport(v);
    root.damageAll();
    root.frame(false);
    const double before = arrange->viewport().pixelsPerSecond;
    ui::PointerEvent wheel{ui::PointerEvent::Type::Wheel, {600, 300}};
    wheel.mods = ui::kCtrl;
    wheel.wheelY = 1;
    root.pointer(wheel);
    CHECK(arrange->viewport().pixelsPerSecond == doctest::Approx(before * 1.25));
    CHECK(arrange->timeAt(600) ==
          doctest::Approx(40 + static_cast<double>(600.0f - arrange->lanesRect().x) / before));
}

TEST_CASE("the TCP column and the lanes stay aligned through scroll and height changes") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    auto aligned = [&] {
        for (std::size_t i = 0; i < arrange->trackLayout().rowCount(); ++i) {
            const ui::Rect lanes = arrange->rowRect(i);
            const ui::Rect tcp = arrange->tcpRowRect(i);
            CHECK(lanes.y == tcp.y);
            CHECK(lanes.h == tcp.h);
            CHECK(lanes.x != tcp.x);  // different columns, same rows
        }
    };
    views::Viewport v;
    v.pixelsPerSecond = 40;
    v.scrollY = 120;
    arrange->setViewport(v);
    root.damageAll();
    root.frame(false);
    aligned();
    arrange->trackLayout().setGlobalHeight(48);
    arrange->trackLayout().setTrackHeight(arrange->trackLayout().rows()[1].track, 120);
    root.damageAll();
    root.frame(false);
    aligned();
    CHECK(arrange->rowRect(1).h == doctest::Approx(120));
    CHECK(arrange->rowRect(0).h == doctest::Approx(48));
}

TEST_CASE("the scrollbars move the view") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    ui::Node* hbar = root.find("hscroll");
    ui::Node* vbar = root.find("vscroll");
    REQUIRE(hbar);
    REQUIRE(vbar);
    // The generated session is longer than the view: the horizontal bar is live.
    const ui::Rect hb = hbar->bounds();
    const double start = arrange->viewport().startSeconds;
    const ui::Point hmid{hb.x + 4, hb.y + hb.h / 2};
    const ui::Point hend{hb.right() - 4, hb.y + hb.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, hmid, 1});
    root.pointer({ui::PointerEvent::Type::Move, hend});
    root.pointer({ui::PointerEvent::Type::Up, hend, 1});
    CHECK(arrange->viewport().startSeconds > start);
    // Taller rows overflow the view: the vertical bar scrolls them.
    arrange->trackLayout().setGlobalHeight(150);
    root.damageAll();
    root.frame(false);
    CHECK(arrange->trackLayout().contentHeight() > arrange->lanesRect().h);
    const ui::Rect vb = vbar->bounds();
    const ui::Point vtop{vb.x + vb.w / 2, vb.y + 4};
    const ui::Point vbottom{vb.x + vb.w / 2, vb.bottom() - 4};
    root.pointer({ui::PointerEvent::Type::Down, vtop, 1});
    root.pointer({ui::PointerEvent::Type::Move, vbottom});
    root.pointer({ui::PointerEvent::Type::Up, vbottom, 1});
    CHECK(arrange->viewport().scrollY > 0);
}

TEST_CASE("the arrange view's saved UI state round-trips") {
    // What the per-project UI file stores (gui.md §1.2): the follow mode by
    // name and the per-track height overrides.
    for (views::Follow mode :
         {views::Follow::Off, views::Follow::Page, views::Follow::Continuous}) {
        const auto name = std::string(views::followName(mode));
        REQUIRE(views::followFrom(name).has_value());
        CHECK(*views::followFrom(name) == mode);
    }
    CHECK_FALSE(views::followFrom("sideways").has_value());
    views::TrackLayout layout;
    layout.setGlobalHeight(48);
    model::Id track{model::Kind::Track, 1};
    layout.setTrackHeight(track, 120);
    REQUIRE(layout.heights().size() == 1);
    CHECK(layout.heights().at(track) == doctest::Approx(120));
    views::TrackLayout restored;
    restored.setGlobalHeight(48);
    for (const auto& [id, h] : layout.heights()) restored.setTrackHeight(id, h);
    CHECK(restored.heights() == layout.heights());
}

TEST_CASE("the playhead overlay moves without damaging the content") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    REQUIRE(arrange->installOverlay(root));
    arrange->setFollow(views::Follow::Off);
    views::Viewport v;
    v.pixelsPerSecond = 40;
    arrange->setViewport(v);
    root.damageAll();
    root.frame(false);
    const std::uint64_t repaints = root.stats().repaints;
    const double area = root.stats().repaintedArea;
    const std::uint64_t frames = root.stats().frames;
    for (int i = 1; i <= 30; ++i) {
        arrange->setPlayhead(0.25 * i);
        CHECK(root.wantsFrame());  // the overlay asks for a frame...
        root.frame(false);
    }
    CHECK(root.stats().repaints == repaints);  // ...that repaints no content
    CHECK(root.stats().repaintedArea == area);
    CHECK(root.stats().frames == frames + 30);
    CHECK(arrange->playhead() == doctest::Approx(7.5));
}

TEST_CASE("keyboard zoom keeps the playhead's time in place; Home and End belong to the keymap") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    root.focus(arrange);
    arrange->setViewport({10, 40, 0});
    arrange->setPlayhead(12.0);
    const float x = arrange->playheadX();
    root.key(SDLK_PLUS);
    CHECK(arrange->viewport().pixelsPerSecond == doctest::Approx(50));
    CHECK(arrange->playheadX() == doctest::Approx(static_cast<double>(x)));
    root.key(SDLK_MINUS);
    CHECK(arrange->viewport().pixelsPerSecond == doctest::Approx(40));
    // The view does not take Home/End: the keymap's transport.seek:start/end
    // keep them, so they must reach Root::onUnhandledKey.
    for (int key : {SDLK_HOME, SDLK_END}) CHECK_FALSE(arrange->onKey(ui::KeyEvent{key, 0, false}));
}

TEST_CASE("the arrangement renders a frame with 1000 generated tracks") {
    Gl& g = gl();
    if (!g.ok) return;
    // Built in one transaction: 1000 items through the bus would decode the
    // same 30 s file 1000 times (item.add reads it per call). The rows, the
    // item index and the draw path are the same the generate path produces.
    model::Session s = model::Session::create();
    model::EditTxn txn(s, 1);
    for (int i = 0; i < 1000; ++i) {
        model::Track t;
        t.type = model::StripType::Channel;
        t.parent = model::kMaster;
        t.name = "Track " + std::to_string(i + 1);
        const model::Id tid = txn.create(t);
        txn.modify<model::Track>(model::kMaster,
                                 [&](model::Track& m) { m.children.push_back(tid); });
        model::MidiSource midi;
        midi.sequence.ppq = 960;
        for (int k = 0; k < 4; ++k) {
            midi::Note note;
            note.start = k * 960;
            note.length = 480;
            note.pitch = static_cast<std::uint8_t>(48 + (i + k) % 24);
            note.velocity = 100;
            midi.sequence.notes.push_back(note);
        }
        const model::Id mi = txn.create(std::move(midi));
        const model::Id take = txn.create(model::Take{});
        txn.modify<model::Take>(take, [&](model::Take& tk) { tk.source = mi; });
        model::Item item;
        item.track = tid;
        item.length = 8;
        item.takes = {take};
        item.activeTake = take;
        txn.create(std::move(item));
    }
    auto committed = txn.commit();
    REQUIRE(committed);
    peaks::PeakService peaks(std::filesystem::temp_directory_path() / "supple-arrange-1000-cache",
                             [](std::function<void()>) {});
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    auto arrange = std::make_unique<views::ArrangeView>(
        peaks, [](const model::MediaSource&) { return std::filesystem::path{}; },
        views::TransportSource{});
    arrange->show(*committed);
    views::ArrangeView* view = arrange.get();
    root.setContent(std::move(arrange));
    views::Viewport v;
    v.pixelsPerSecond = 40;
    view->setViewport(v);
    root.damageAll();
    root.frame(false);
    CHECK(view->trackLayout().rowCount() == 1000);
    CHECK(view->lanes().stats().rows > 0);
    CHECK(view->lanes().stats().rows < 20);  // only what fits: virtualized
}

TEST_CASE("locked items are marked") {
    Gl& g = gl();
    if (!g.ok) return;
    auto build = [](bool locked) {
        model::Session s = model::Session::create();
        model::EditTxn txn(s, 1);
        model::Track track;
        track.type = model::StripType::Channel;
        track.parent = model::kMaster;
        track.name = "T";
        const model::Id tid = txn.create(track);
        txn.modify<model::Track>(model::kMaster,
                                 [&](model::Track& m) { m.children.push_back(tid); });
        model::Item item;
        item.track = tid;
        item.length = 4;
        item.locked = locked;
        item.fadeIn.length = 0.25;
        const model::Id iid = txn.create(item);
        const model::Id take = txn.create(model::Take{});
        txn.modify<model::Item>(iid, [&](model::Item& i) {
            i.takes.push_back(take);
            i.activeTake = take;
        });
        auto committed = txn.commit();
        REQUIRE(committed);
        return *committed;
    };
    peaks::PeakService peaks(std::filesystem::temp_directory_path() / "supple-arrange-lock-cache",
                             [](std::function<void()>) {});
    auto render = [&](const model::Session& s) {
        ui::Root root(*g.window, g.fonts, ui::Theme::dark());
        auto arrange = std::make_unique<views::ArrangeView>(
            peaks, [](const model::MediaSource&) { return std::filesystem::path{}; },
            views::TransportSource{});
        arrange->show(s);
        arrange->setViewport({0, 60, 0});
        arrange->setFollow(views::Follow::Off);
        root.setContent(std::move(arrange));
        root.damageAll();
        root.frame(false);
        return root.readBack();
    };
    const gui::Image plain = render(build(false));
    const gui::Image marked = render(build(true));
    std::size_t diff = 0;
    for (std::size_t i = 0; i < plain.rgba.size(); i += 4)
        if (std::abs(int(plain.rgba[i]) - int(marked.rgba[i])) > 8) ++diff;
    CHECK(diff > 0);  // the padlock paints somewhere
}

// --- golden images ------------------------------------------------------------------

TEST_CASE("golden: the ruler at three zoom levels") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    arrange->installOverlay(root);
    arrange->setFollow(views::Follow::Off);
    arrange->setPlayhead(3.456);
    auto shot = [&](double pps, const char* name) {
        views::Viewport v;
        v.startSeconds = 0;
        v.pixelsPerSecond = pps;
        arrange->setViewport(v);
        settle(root, arrange->lanes());
        golden(root, name);
    };
    shot(4, "arrange-ruler-bars.png");
    shot(160, "arrange-ruler-beats.png");
    shot(400, "arrange-ruler-subbeats.png");
}

TEST_CASE("golden: a session with tempo and meter changes") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    arrange->installOverlay(root);
    arrange->setFollow(views::Follow::Off);
    arrange->setPlayhead(66.0);
    views::Viewport v;
    v.startSeconds = 0;
    v.pixelsPerSecond = 6;  // bar 33 (140 BPM) and bar 35 (3/4) are in view
    arrange->setViewport(v);
    settle(root, arrange->lanes());
    golden(root, "arrange-tempo-meter.png");
}

TEST_CASE("golden: markers, a region and the loop range") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    arrange->installOverlay(root);
    arrange->setFollow(views::Follow::Off);
    arrange->setPlayhead(4.0);
    arrange->setLoopRange(8.0, 24.0, true);
    views::Viewport v;
    v.startSeconds = 0;
    v.pixelsPerSecond = 12;
    arrange->setViewport(v);
    settle(root, arrange->lanes());
    golden(root, "arrange-markers-regions.png");
}

TEST_CASE("golden: items with fades, muted items and multi-take items") {
    Gl& g = gl();
    if (!g.ok) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    views::ArrangeView* arrange = mount(root);
    arrange->installOverlay(root);
    arrange->setFollow(views::Follow::Off);
    arrange->setPlayhead(2.0);
    views::Viewport v;
    v.startSeconds = 0;
    v.pixelsPerSecond = 40;
    arrange->setViewport(v);
    settle(root, arrange->lanes());
    golden(root, "arrange-items.png");
}

// --- the track control panel (P5.5) -------------------------------------------------

namespace {

// Mounts an arrange view whose TCP commits through the fixture's bus, the way
// the shell wires it (selection in a vector, Origin::Ui, drag gestures).
struct TcpHarness {
    Session* session = nullptr;
    std::vector<std::string> selected;
    std::string error;
    std::vector<std::string> gestures;  // every commit's coalescing key (tests)
    std::optional<std::string> anchor;
    gui::MeterFeed* feed = nullptr;
    views::ArrangeView* view = nullptr;

    views::ArrangeView* mount(ui::Root& root, Session& s, gui::MeterFeed* meters = nullptr) {
        session = &s;
        feed = meters;
        auto arrange =
            std::make_unique<views::ArrangeView>(*s.peaks, s.resolve(), views::TransportSource{});
        views::TcpCallbacks cb;
        cb.selected = [this](const model::Id& id) {
            return std::find(selected.begin(), selected.end(), id.str()) != selected.end();
        };
        cb.select = [this](const model::Id& id, unsigned mods) { select(id, mods); };
        cb.commit = [this](std::string_view command, json::Value args, std::string gesture) {
            gestures.push_back(gesture);
            cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
            origin.gesture = std::move(gesture);
            auto r = session->dispatcher->execute(
                {std::string(command), std::move(args), std::move(origin)});
            if (!r) error = r.error().code;
        };
        cb.addStrip = [this](std::string_view type) {
            json::Value args = json::Value::object();
            args["type"] = std::string(type);
            cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
            auto r =
                session->dispatcher->execute({"track.add", std::move(args), std::move(origin)});
            if (!r) {
                error = r.error().code;
                return;
            }
            selected = {(*r)["track"].asString()};  // the new strip is selected
            anchor = selected.front();
        };
        if (meters) {
            cb.meterShown = [meters](const std::string& track, ui::Meter* meter) {
                meters->attach(track, meter);
            };
            cb.meterHidden = [meters](ui::Meter* meter) { meters->detach(meter); };
        }
        arrange->setTcpCallbacks(std::move(cb));
        arrange->show(s.app->head());
        views::ArrangeView* view_ = arrange.get();
        view = view_;
        root.setContent(std::move(arrange));
        return view_;
    }

    void select(const model::Id& id, unsigned mods) {
        const std::string key = id.str();
        if (mods & ui::kShift) {  // extend from the anchor, in row order
            const auto& rows = view->trackLayout().rows();
            auto index = [&](const std::string& want) {
                for (std::size_t i = 0; i < rows.size(); ++i)
                    if (rows[i].track.str() == want) return static_cast<int>(i);
                return -1;
            };
            const std::string from = anchor.value_or(key);
            const int a = index(from), b = index(key);
            if (a >= 0 && b >= 0) {
                selected.clear();
                for (int i = std::min(a, b); i <= std::max(a, b); ++i)
                    selected.push_back(rows[static_cast<std::size_t>(i)].track.str());
            } else {
                selected = {key};
            }
        } else if (mods & ui::kCtrl) {
            const auto it = std::find(selected.begin(), selected.end(), key);
            if (it == selected.end())
                selected.push_back(key);
            else
                selected.erase(it);
        } else {
            selected = {key};
        }
        anchor = key;
    }
    // The number of versions on the branch (history.list), like the brief's
    // "one undo step" check.
    std::size_t versions() {
        auto r = session->dispatcher->execute(
            {"history.list", json::Value::object(), {cmd::Origin::Type::Test, "gui-test", {}}});
        REQUIRE(r);
        return (*r)["versions"].asArray().size();
    }
    bool is(const char* track) const {
        return std::find(selected.begin(), selected.end(), std::string(track)) != selected.end();
    }
};

// Clicks the middle of a node through the root (the same path SDL input takes).
void press(ui::Root& root, ui::Node* node, int clicks = 1, unsigned mods = 0) {
    const ui::Rect b = node->bounds();
    const ui::Point at{b.x + b.w / 2, b.y + b.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, at, 1, clicks, mods});
    root.pointer({ui::PointerEvent::Type::Up, at, 1, clicks, mods});
}

double trackDb(Session& s, const char* id) {
    return s.app->head().tracks.find(*model::Id::parse(id))->volumeDb;
}

}  // namespace

TEST_CASE("the TCP renames a strip in place through the bus") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    views::Viewport v;
    v.pixelsPerSecond = 40;
    arrange->setViewport(v);
    root.damageAll();
    root.frame(false);
    const model::Id tr1 = *model::Id::parse("tr_1");
    ui::Node* row = arrange->tcpPanel().rowNode(tr1);
    REQUIRE(row);
    // Double-click the name: a text field appears, focused, with the old name.
    const ui::Rect name = arrange->tcpPanel().controlNode(tr1, "rename")->bounds();
    const ui::Point at{name.x + 8, name.y + name.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, at, 1, 2});
    root.pointer({ui::PointerEvent::Type::Up, at, 1, 2});
    ui::Node* field = arrange->tcpPanel().controlNode(tr1, "rename");
    REQUIRE(field);
    CHECK(field->visible);
    CHECK(root.focused() == field);
    root.text("Bass");
    root.key(SDLK_RETURN);
    CHECK(tcp.error.empty());
    CHECK(s.app->head().tracks.find(tr1)->name == "Bass");
    CHECK_FALSE(arrange->tcpPanel().controlNode(tr1, "rename")->visible);
}

TEST_CASE("the TCP toggles mute, solo, arm and monitor on the strip") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    const model::Id tr2 = *model::Id::parse("tr_2");
    auto* panel = &arrange->tcpPanel();
    // The app's loop syncs the panel to the new version after every command.
    auto sync = [&] {
        arrange->show(s.app->head());
        root.damageAll();
        root.frame(false);
    };
    REQUIRE(panel->controlNode(tr2, "mute"));
    press(root, panel->controlNode(tr2, "mute"));
    CHECK(s.app->head().tracks.find(tr2)->mute);
    sync();
    press(root, panel->controlNode(tr2, "solo"));
    CHECK(s.app->head().tracks.find(tr2)->solo == model::Solo::On);
    sync();
    press(root, panel->controlNode(tr2, "arm"));
    CHECK(s.app->head().tracks.find(tr2)->record.armed);
    sync();
    press(root, panel->controlNode(tr2, "monitor"));  // Off -> On
    CHECK(s.app->head().tracks.find(tr2)->record.monitor == model::Monitor::On);
    sync();
    press(root, panel->controlNode(tr2, "monitor"));  // On -> Auto
    CHECK(s.app->head().tracks.find(tr2)->record.monitor == model::Monitor::Auto);
    sync();
    press(root, panel->controlNode(tr2, "monitor"));  // Auto -> Off
    CHECK(s.app->head().tracks.find(tr2)->record.monitor == model::Monitor::Off);
    CHECK(tcp.error.empty());
    // A master has neither arm nor monitor.
    const model::Id master = model::kMaster;
    CHECK(panel->controlNode(master, "arm") == nullptr);  // the master is not a row
}

TEST_CASE("a fader drag is one undo step and lands on the moved value") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    const model::Id tr1 = *model::Id::parse("tr_1");
    ui::Node* fader = arrange->tcpPanel().controlNode(tr1, "fader");
    REQUIRE(fader);
    const ui::Rect fb = fader->bounds();
    const std::size_t before = tcp.versions();
    const double startDb = trackDb(s, "tr_1");
    const ui::Point at{fb.x + fb.w / 2, fb.y + fb.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, at, 1});
    for (int i = 1; i <= 20; ++i)  // 20 moves of one drag
        root.pointer({ui::PointerEvent::Type::Move, {at.x - static_cast<float>(i) * 2, at.y}});
    root.pointer({ui::PointerEvent::Type::Up, at, 1});
    CHECK(tcp.error.empty());
    const model::Id tr1b = tr1;
    const double moved = s.app->head().tracks.find(tr1b)->volumeDb;
    CHECK(moved < startDb);  // dragged left: quieter
    // Every move of the drag carried the same coalescing key.
    REQUIRE(tcp.gestures.size() == 20);
    for (const std::string& key : tcp.gestures) CHECK(key == tcp.gestures.front());
    CHECK(tcp.gestures.front().find("tcp.volume:tr_1:") == 0);
    CHECK(tcp.versions() == before + 1);  // one undo step for the whole drag
    // A second drag is a second step, not a continuation of the first.
    const std::size_t afterFirst = tcp.versions();
    root.pointer({ui::PointerEvent::Type::Down, at, 1});
    root.pointer({ui::PointerEvent::Type::Move, {at.x - 4, at.y}});
    root.pointer({ui::PointerEvent::Type::Up, at, 1});
    CHECK(tcp.versions() == afterFirst + 1);
    // A double-click resets to unity, as its own edit.
    root.pointer({ui::PointerEvent::Type::Down, at, 1, 2});
    root.pointer({ui::PointerEvent::Type::Up, at, 1, 2});
    CHECK(trackDb(s, "tr_1") == doctest::Approx(0.0));
    CHECK(tcp.versions() == afterFirst + 2);
}

TEST_CASE("a pan drag is one undo step") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    const model::Id tr3 = *model::Id::parse("tr_3");
    ui::Node* pan = arrange->tcpPanel().controlNode(tr3, "pan");
    REQUIRE(pan);
    const ui::Rect pb = pan->bounds();
    const std::size_t before = tcp.versions();
    const ui::Point at{pb.x + pb.w / 2, pb.y + pb.h / 2};
    root.pointer({ui::PointerEvent::Type::Down, at, 1});
    for (int i = 1; i <= 6; ++i)
        root.pointer({ui::PointerEvent::Type::Move, {at.x + static_cast<float>(i) * 3, at.y}});
    root.pointer({ui::PointerEvent::Type::Up, at, 1});
    CHECK(tcp.error.empty());
    CHECK(s.app->head().tracks.find(tr3)->pan > 0.0);
    CHECK(tcp.versions() == before + 1);
}

TEST_CASE("TCP selection follows click, Ctrl+click and Shift+click") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    auto* panel = &arrange->tcpPanel();
    const model::Id tr1 = *model::Id::parse("tr_1");
    const model::Id tr3 = *model::Id::parse("tr_3");
    REQUIRE(panel->rowNode(tr1));
    press(root, panel->rowNode(tr1));
    CHECK(tcp.is("tr_1"));
    CHECK(tcp.selected.size() == 1);
    press(root, panel->rowNode(tr3), 1, ui::kCtrl);
    CHECK(tcp.is("tr_1"));
    CHECK(tcp.is("tr_3"));
    press(root, panel->rowNode(tr3), 1, ui::kCtrl);  // toggles off
    CHECK_FALSE(tcp.is("tr_3"));
    press(root, panel->rowNode(tr1));
    press(root, panel->rowNode(tr3), 1, ui::kShift);  // extends
    CHECK(tcp.selected.size() == 3);                  // tr_1..tr_3
    CHECK(tcp.error.empty());
}

TEST_CASE("golden: the TCP with one strip of each type") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    const cmd::Origin test{cmd::Origin::Type::Test, "gui-test", {}};
    auto run = [&](const char* command, json::Value args) {
        auto r = s.dispatcher->execute({command, std::move(args), test});
        REQUIRE(r);
        return *r;
    };
    // A buss with a channel under it: the parentage bar and indent show.
    json::Value bussArgs = json::Value::object();
    bussArgs["type"] = "buss";
    bussArgs["name"] = "Drum bus";
    const std::string buss = run("track.add", std::move(bussArgs))["track"].asString();
    json::Value childArgs = json::Value::object();
    childArgs["name"] = "Kick";
    childArgs["parent"] = buss;
    const std::string child = run("track.add", std::move(childArgs))["track"].asString();
    // One muted strip, one soloed, one selected.
    json::Value mute = json::Value::object();
    mute["track"] = buss;
    mute["mute"] = true;
    run("track.set", std::move(mute));
    json::Value solo = json::Value::object();
    solo["track"] = child;
    solo["solo"] = "on";
    run("track.set", std::move(solo));
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    tcp.selected = {child};  // and the child is selected
    views::Viewport v;
    v.pixelsPerSecond = 40;
    v.scrollY = 1e6f;  // the buss and its child are the last rows
    arrange->setViewport(v);
    root.damageAll();
    root.frame(false);
    golden(root, "tcp-strips.png");
}

TEST_CASE("the TCP's + offers exactly Channel, Buss and Master") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    REQUIRE(root.find("tcp.add"));
    for (auto [index, type] : {std::pair{0, model::StripType::Channel},
                               {1, model::StripType::Buss},
                               {2, model::StripType::Master}}) {
        press(root, root.find("tcp.add"));
        auto* menu = static_cast<ui::List*>(root.find("tcp.addmenu"));
        REQUIRE(menu);
        CHECK(menu->count() == 3);  // exactly the three strip types
        const std::size_t before = s.app->head().tracks.size();
        menu->onActivate(static_cast<std::size_t>(index));
        CHECK(s.app->head().tracks.size() == before + 1);
        REQUIRE(tcp.selected.size() == 1);
        const model::Track* created = s.app->head().tracks.find(*model::Id::parse(tcp.selected[0]));
        REQUIRE(created);
        CHECK(created->type == type);
    }
    CHECK(tcp.error.empty());
}

TEST_CASE("the TCP meters take readings from the feed and hold peaks") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    std::vector<gui::StripReading> readings;
    gui::MeterFeed feed([&readings] { return readings; });
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s, &feed);
    arrange->setViewport({0, 40, 0});
    root.damageAll();
    root.frame(false);
    const model::Id tr1 = *model::Id::parse("tr_1");
    ui::Meter* meter = static_cast<ui::Meter*>(arrange->tcpPanel().controlNode(tr1, "meter"));
    REQUIRE(meter);
    // A reading for tr_1 (and one for a track the panel does not show).
    gui::StripReading r;
    r.track = "tr_1";
    r.peak[0] = r.peak[1] = 0.5f;  // -6 dBFS
    r.rms[0] = r.rms[1] = 0.25f;
    readings = {r};
    feed.poll();
    CHECK(meter->shownPeakDb(0) == doctest::Approx(static_cast<double>(-6.02f)).epsilon(0.2));
    // The hold keeps it for a while, then the decay ticks it down.
    readings.clear();
    feed.poll();
    const float held = meter->shownPeakDb(0);
    CHECK(held > -10.0f);
    for (int i = 0; i < 30; ++i) feed.poll();
    CHECK(meter->shownPeakDb(0) < held);
}

TEST_CASE("the meters repaint only themselves, not the arrange content") {
    Gl& g = gl();
    if (!g.ok) return;
    Session& s = session6();
    if (!s.app) return;
    std::vector<gui::StripReading> readings;
    gui::MeterFeed feed([&readings] { return readings; });
    ui::Root root(*g.window, g.fonts, ui::Theme::dark());
    TcpHarness tcp;
    views::ArrangeView* arrange = tcp.mount(root, s, &feed);
    arrange->setViewport({0, 40, 0});
    root.frame(false);
    const std::uint64_t repaints = root.stats().repaints;
    const double area = root.stats().repaintedArea;
    const model::Id tr1 = *model::Id::parse("tr_1");
    ui::Meter* meter = static_cast<ui::Meter*>(arrange->tcpPanel().controlNode(tr1, "meter"));
    REQUIRE(meter);
    ui::Rect meterRect = meter->bounds();
    gui::StripReading r;
    r.track = "tr_1";
    r.peak[0] = r.peak[1] = 0.7f;
    readings = {r};
    feed.poll();
    root.frame(false);
    // Only the meter's own rectangle (or a little more) was repainted.
    CHECK(root.stats().repaintedArea - area < 4000.0);
    CHECK(root.stats().repaintedArea - area > 0.0);
    (void)repaints;
    (void)meterRect;
}
