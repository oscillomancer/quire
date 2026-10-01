// The lanes view over a real session built through the command bus (the
// same commands an agent uses), drawn offscreen.
#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <stb_image.h>

#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include "../app/generate.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/ui/root.h"
#include "supple/views/lanes.h"

using namespace supple;

namespace {

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    std::vector<std::function<void()>> posted;
    std::mutex m;
    std::unique_ptr<peaks::PeakService> peaks;
    bool ok = false;

    Fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-views-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"views-test", 800, 400, true});
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
        app->attachCommands(*dispatcher);
        REQUIRE(gui::generateSession(*dispatcher, app->mediaDir(), 4));
        peaks =
            std::make_unique<peaks::PeakService>(dir / "cache", [this](std::function<void()> fn) {
                std::lock_guard lock(m);
                posted.push_back(std::move(fn));
            });
        ok = true;
    }
    ~Fixture() {
        peaks.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    void drain() {
        std::vector<std::function<void()>> now;
        {
            std::lock_guard lock(m);
            now.swap(posted);
        }
        for (auto& fn : now) fn();
    }
    // Frames until the view has every visible pyramid.
    void settle(ui::Root& root, views::Lanes& lanes) {
        for (int i = 0; i < 400; ++i) {
            drain();
            root.damageAll();
            root.frame(false);
            if (lanes.stats().pending == 0) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        FAIL("peaks never became ready");
    }
};

Fixture& fixture() {
    static Fixture f;
    return f;
}

views::Lanes& mount(ui::Root& root) {
    Fixture& f = fixture();
    auto lanes = std::make_unique<views::Lanes>(*f.peaks, [&f](const model::MediaSource& m) {
        const std::filesystem::path p(m.path);
        return p.is_absolute() ? p : f.app->mediaDir() / p;
    });
    views::Lanes& ref = *lanes;
    root.setContent(std::move(lanes));
    ref.show(f.app->head());
    return ref;
}

}  // namespace

TEST_CASE("the lanes show every strip's items where the engine plays them") {
    Fixture& f = fixture();
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    ui::Root root(*f.window, f.fonts, ui::Theme::dark());
    views::Lanes& lanes = mount(root);
    views::Viewport v;
    v.startSeconds = 0;
    v.pixelsPerSecond = 12;  // 800 px ≈ 66 s: everything
    v.rowHeight = 90;
    lanes.setViewport(v);
    f.settle(root, lanes);
    CHECK(lanes.stats().rows == 4);
    CHECK(lanes.stats().items == 8);         // an audio and a MIDI item per strip
    CHECK(lanes.stats().notes == 4 * 64);    // 8 bars of eighths per MIDI item
    CHECK(lanes.stats().columns > 4 * 300);  // two channels per audio item
    CHECK(f.peaks->stats().built == 4);      // four distinct files (tracks 1-4)
}

TEST_CASE("rows and items outside the viewport are not drawn") {
    Fixture& f = fixture();
    if (!f.ok) return;
    ui::Root root(*f.window, f.fonts, ui::Theme::dark());
    views::Lanes& lanes = mount(root);
    views::Viewport v;
    v.startSeconds = 40;  // the MIDI items only (bar 21 = 40 s at 120 BPM)
    v.pixelsPerSecond = 40;
    v.rowHeight = 150;
    v.scrollY = 150;  // first row scrolled away; rows 2-3 visible, 4 cut off
    lanes.setViewport(v);
    f.settle(root, lanes);
    CHECK(lanes.stats().rows == 3);
    CHECK(lanes.stats().items == 3);
    CHECK(lanes.stats().columns == 0);
    CHECK(lanes.stats().notes > 0);
    // Wheel scrolls rows; ctrl+wheel zooms around the pointer.
    const double before = lanes.viewport().pixelsPerSecond;
    ui::PointerEvent wheel{ui::PointerEvent::Type::Wheel, {400, 200}};
    wheel.mods = ui::kCtrl;
    wheel.wheelY = 1;
    root.pointer(wheel);
    CHECK(lanes.viewport().pixelsPerSecond == doctest::Approx(before * 1.25));
    CHECK(lanes.viewport().startSeconds + 400 / lanes.viewport().pixelsPerSecond ==
          doctest::Approx(40 + 400 / before));  // the time under the pointer stays put
}

TEST_CASE("golden image: audio tails and MIDI items") {
    Fixture& f = fixture();
    if (!f.ok) return;
    ui::Root root(*f.window, f.fonts, ui::Theme::dark());
    views::Lanes& lanes = mount(root);
    views::Viewport v;
    v.startSeconds = 24;
    v.pixelsPerSecond = 25;
    v.rowHeight = 96;
    lanes.setViewport(v);
    f.settle(root, lanes);
    const gui::Image image = root.readBack();
    const auto golden = std::filesystem::path(SUPPLE_GUI_GOLDEN_DIR) / "lanes-dark.png";
    if (std::getenv("SUPPLE_UPDATE_GOLDEN") || !std::filesystem::exists(golden)) {
        REQUIRE(gui::writePng(golden, image));
        MESSAGE("golden written: " << golden.string());
        return;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(golden.c_str(), &w, &h, &n, 4);
    REQUIRE(data);
    REQUIRE(w == image.width);
    REQUIRE(h == image.height);
    // Per-pixel tolerance for driver antialiasing; count pixels that differ a lot.
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
