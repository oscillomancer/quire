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

#include "generate.h"
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

// Frames until the view has every visible pyramid.
void settle(ui::Root& root, views::Lanes& lanes) {
    for (int i = 0; i < 400; ++i) {
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
    // No two labels ever overlap: the drawn gaps stay above the thresholds.
    CHECK(bars.minBarLabelGap >= 40.0f);
    CHECK(beats.minBarLabelGap >= 40.0f);
    CHECK(subs.minBarLabelGap >= 40.0f);
    CHECK(bars.minTimeLabelGap >= 55.0f);
    CHECK(subs.minTimeLabelGap >= 55.0f);
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

TEST_CASE("keyboard zoom keeps the playhead's time in place; Home and End scroll") {
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
    root.key(SDLK_HOME);
    CHECK(arrange->viewport().startSeconds == doctest::Approx(0));
    root.key(SDLK_END);
    CHECK(arrange->viewport().startSeconds > 0);
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
