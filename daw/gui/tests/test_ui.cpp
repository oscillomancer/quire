#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <stb_image.h>

#include <cstdlib>
#include <filesystem>

#include "supple/gui/png.h"
#include "supple/json/parse.h"
#include "supple/ui/root.h"
#include "supple/ui/widgets.h"

using namespace supple;
using namespace supple::ui;

namespace {

// One offscreen window shared by the tests (GL contexts are not free).
struct Fixture {
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    bool ok = false;
    Fixture() {
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"ui-test", 320, 200, true});
        if (!w) return;
        window = std::move(*w);
        auto f = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        if (!f) return;
        fonts = *f;
        ok = true;
    }
};

Fixture& fixture() {
    static Fixture f;
    return f;
}

#define REQUIRE_GL()                                             \
    if (!fixture().ok) {                                         \
        MESSAGE("skipped: no offscreen GL in this environment"); \
        return;                                                  \
    }

Node& panel(Node& n, std::string id, Layout::Dir dir) {
    auto& p = n.emplace<Node>(std::move(id));
    p.layout.dir = dir;
    return p;
}

// A filled box that records how often it paints.
struct Box : Node {
    explicit Box(std::string id, std::string token = "raised")
        : Node(std::move(id)), color(std::move(token)) {}
    std::string color;
    int paints = 0;
    void paint(Canvas& c) override {
        ++paints;
        c.fillRect(bounds(), c.theme().color(color));
    }
};

double pixelDistance(const gui::Image& a, const gui::Image& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    double worst = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i)
        worst = std::max(worst,
                         std::abs(static_cast<double>(a.rgba[i]) - static_cast<double>(b.rgba[i])));
    return worst;
}

}  // namespace

TEST_CASE("stack layout: fixed, content and fractions with padding, gaps and alignment") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Column;
    content->layout.padding = Insets::all(10);
    content->layout.gap = 4;
    auto& top = content->emplace<Box>("top");
    top.layout.height = SizeRule::fixed(30);
    auto& row = panel(*content, "row", Layout::Dir::Row);  // takes the rest
    row.layout.gap = 2;
    auto& a = row.emplace<Box>("a");
    a.layout.width = SizeRule::fraction(1);
    auto& b = row.emplace<Box>("b");
    b.layout.width = SizeRule::fraction(3);
    auto& c = row.emplace<Box>("c");
    c.layout.width = SizeRule::fixed(50);
    c.layout.height = SizeRule::fixed(20);
    row.layout.align = Layout::Align::Center;
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(top.bounds() == Rect{10, 10, 300, 30});
    CHECK(row.bounds() == Rect{10, 44, 300, 146});
    // 300 − 50 fixed − 2 × 2 gap = 246 shared 1:3.
    CHECK(a.bounds().w == doctest::Approx(61.5));
    CHECK(b.bounds().w == doctest::Approx(184.5));
    CHECK(c.bounds().x == doctest::Approx(10 + 61.5 + 2 + 184.5 + 2));
    CHECK(c.bounds().y == doctest::Approx(44 + (146 - 20) / 2.0));  // centred cross axis
    CHECK(a.bounds().h == doctest::Approx(146));                    // fraction cross: stretch
}

TEST_CASE("a label sizes to its text; a button sizes to its label plus padding") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    auto& label = content->emplace<Label>("label", "Tempo");
    auto& button = content->emplace<Button>("button", "Render…");
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(label.bounds().w > 20);
    CHECK(label.bounds().w < 80);
    CHECK(button.bounds().w > label.bounds().w);
    CHECK(button.bounds().h == 28);
    label.setText("A much longer tempo label");
    root.frame(false);
    CHECK(label.bounds().w > 120);  // content size follows the text
}

TEST_CASE("damage repaints only what changed") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    auto& left = content->emplace<Box>("left");
    auto& right = content->emplace<Box>("right", "panel");
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(left.paints == 1);
    CHECK(right.paints == 1);
    CHECK_FALSE(root.wantsFrame());
    const double before = root.stats().repaintedArea;
    right.invalidate();
    CHECK(root.wantsFrame());
    root.frame(false);
    CHECK(left.paints == 1);  // culled: outside the damage
    CHECK(right.paints == 2);
    CHECK(root.stats().repaintedArea - before == doctest::Approx(160.0 * 200.0));
    // The cache keeps the undamaged half: the frame matches a full repaint.
    const gui::Image partial = root.readBack();
    root.damageAll();
    root.frame(false);
    CHECK(pixelDistance(partial, root.readBack()) == 0);
}

TEST_CASE("clicks, focus traversal and keys reach the right nodes") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Row;
    content->layout.gap = 8;
    int ok = 0, cancel = 0;
    content->emplace<Button>("ok", "OK", [&] { ++ok; });
    auto& cancelButton = content->emplace<Button>("cancel", "Cancel", [&] { ++cancel; });
    root.setContent(std::move(content));
    root.frame(false);
    CHECK(root.click("ok"));
    CHECK(ok == 1);
    CHECK(root.focused() == root.find("ok"));  // a click focuses a focusable target
    root.key(SDLK_TAB);
    CHECK(root.focused() == &cancelButton);
    root.key(SDLK_RETURN);
    CHECK(cancel == 1);
    root.key(SDLK_TAB, kShift);
    CHECK(root.focused() == root.find("ok"));
    CHECK_FALSE(root.click("missing"));
    cancelButton.enabled = false;
    CHECK_FALSE(root.click("cancel"));
    CHECK(cancel == 1);
    // A press that ends outside the button does not click it.
    const Rect b = root.find("ok")->bounds();
    root.pointer({PointerEvent::Type::Down, {b.x + 2, b.y + 2}, 1});
    root.pointer({PointerEvent::Type::Up, {b.x + 200, b.y + 2}, 1});
    CHECK(ok == 1);
}

TEST_CASE("theme JSON overrides tokens and reports bad values with their path") {
    auto doc = json::parse(R"({"schema": "supple.theme/1.0", "name": "test",
        "colors": {"accent": "#102030"}, "space": {"m": 20}})");
    REQUIRE(doc);
    auto theme = Theme::fromJson(*doc, Theme::dark());
    REQUIRE(theme);
    CHECK(theme->name() == "test");
    CHECK(theme->color("accent") == Color::hex(0x102030));
    CHECK(theme->color("ink") == Theme::dark().color("ink"));  // untouched tokens keep the base
    CHECK(theme->space("m") == 20);
    CHECK(theme->color("no-such-token") == Color::hex(0xff00ff));
    auto bad = json::parse(R"({"schema": "supple.theme/1.0", "colors": {"accent": "blue"}})");
    auto rejected = Theme::fromJson(*bad, Theme::dark());
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().path == "/colors/accent");
}

TEST_CASE("golden image: a small panel in the dark theme") {
    REQUIRE_GL();
    Root root(*fixture().window, fixture().fonts, Theme::dark());
    auto content = std::make_unique<Node>("root");
    content->layout.dir = Layout::Dir::Column;
    content->layout.padding = Insets::all(16);
    content->layout.gap = 12;
    content->emplace<Label>("title", "Render", Font::UiBold, "title");
    auto& row = panel(*content, "row", Layout::Dir::Row);
    row.layout.height = SizeRule::fixed(28);
    row.layout.gap = 8;
    row.emplace<Button>("go", "Render 1 file");
    row.emplace<Button>("dry", "Dry run");
    content->emplace<Label>("hint", "renders/song.wav", Font::Mono, "mono", "ink-quiet");
    root.setContent(std::move(content));
    root.frame(false);
    const gui::Image image = root.readBack();
    const std::filesystem::path golden =
        std::filesystem::path(SUPPLE_GUI_GOLDEN_DIR) / "panel-dark.png";
    if (std::getenv("SUPPLE_UPDATE_GOLDEN") || !std::filesystem::exists(golden)) {
        REQUIRE(gui::writePng(golden, image));
        MESSAGE("golden written: " << golden.string());
        return;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(golden.c_str(), &w, &h, &n, 4);
    REQUIRE(data);
    gui::Image expected{
        w, h, std::vector<std::uint8_t>(data, data + static_cast<std::size_t>(w * h * 4))};
    stbi_image_free(data);
    // Antialiasing may differ by driver; layout or colour regressions do not.
    CHECK(pixelDistance(image, expected) <= 48);
}
