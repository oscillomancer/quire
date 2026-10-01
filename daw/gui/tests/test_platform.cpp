#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <nanovg.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <thread>

#include "supple/gui/png.h"
#include "supple/gui/window.h"

using namespace supple;

TEST_CASE("the PNG writer produces files a decoder reads back exactly") {
    gui::Image image;
    image.width = 300;  // > one 65535-byte stored block per image
    image.height = 120;
    image.rgba.resize(static_cast<std::size_t>(image.width * image.height * 4));
    for (std::size_t i = 0; i < image.rgba.size(); ++i)
        image.rgba[i] = static_cast<std::uint8_t>((i * 31 + i / 7) & 0xff);
    const auto png = gui::encodePng(image);
    int w = 0, h = 0, channels = 0;
    unsigned char* decoded =
        stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4);
    REQUIRE(decoded != nullptr);
    CHECK(w == image.width);
    CHECK(h == image.height);
    CHECK(std::equal(image.rgba.begin(), image.rgba.end(), decoded));
    stbi_image_free(decoded);
}

TEST_CASE("an offscreen window draws with GL and NanoVG and reads back its pixels") {
    auto platform = gui::Platform::start(gui::Platform::Driver::Offscreen);
    if (!platform) {
        MESSAGE("skipped: no offscreen GL here: " << platform.error().message);
        return;
    }
    auto window = gui::Window::open(**platform, {"test", 64, 48, true});
    REQUIRE_MESSAGE(window, (window ? "" : window.error().message));
    gui::Window& w = **window;
    CHECK(w.x11Window() == 0);  // offscreen: nothing to parent plugin editors to
    auto fonts = gui::loadFonts(w.vg(), SUPPLE_GUI_FONTS_DIR);
    REQUIRE_MESSAGE(fonts, (fonts ? "" : fonts.error().message));
    w.beginFrame(0x102030);
    nvgBeginPath(w.vg());
    nvgRect(w.vg(), 0, 0, 16, 16);  // top-left corner in design units
    nvgFillColor(w.vg(), nvgRGB(250, 10, 20));
    nvgFill(w.vg());
    w.endFrame();
    const gui::Image image = w.readPixels();
    w.present();
    REQUIRE(image.width >= 64);
    auto at = [&](int x, int y) {
        const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                        static_cast<std::size_t>(x)) *
                       4;
        return std::array<int, 3>{image.rgba[i], image.rgba[i + 1], image.rgba[i + 2]};
    };
    CHECK(at(4, 4) == std::array<int, 3>{250, 10, 20});  // rows are top to bottom
    CHECK(at(image.width - 4, image.height - 4) == std::array<int, 3>{0x10, 0x20, 0x30});
}

TEST_CASE("the platform wake event can be pushed from another thread") {
    auto platform = gui::Platform::start(gui::Platform::Driver::Offscreen);
    if (!platform) return;
    std::thread([&] { (*platform)->wake(); }).join();
    SDL_Event e;
    bool woke = false;
    while (SDL_WaitEventTimeout(&e, 100))
        if (e.type == (*platform)->wakeEvent()) {
            woke = true;
            break;
        }
    CHECK(woke);
}
