// The arrange view's editing gestures (P5.6), driven like a user: pointer
// events through the root; the model read back; one history entry each.
#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <mutex>

#include "generate.h"
#include "plugin_scales.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"
#include "supple/gui/window.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/root.h"
#include "supple/views/arrange.h"
#include "supple/views/envelope_lane.h"
#include "supple/views/tcp.h"

using namespace supple;

namespace {

const cmd::Origin kUi{cmd::Origin::Type::Ui, "gui", {}};

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<gui::Platform> platform;
    std::unique_ptr<gui::Window> window;
    gui::Fonts fonts;
    std::unique_ptr<engine::EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> dispatcher;
    std::unique_ptr<peaks::PeakService> peaks;
    std::unique_ptr<ui::Root> root;
    views::ArrangeView* view = nullptr;
    std::vector<model::Id> opened;
    std::vector<double> seeks;
    bool ok = false;

    Fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("supple-gesture-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        auto p = gui::Platform::start(gui::Platform::Driver::Offscreen);
        if (!p) return;
        platform = std::move(*p);
        auto w = gui::Window::open(*platform, {"gesture-test", 1100, 600, true});
        if (!w) return;
        window = std::move(*w);
        auto f = gui::loadFonts(window->vg(), SUPPLE_GUI_FONTS_DIR);
        if (!f) return;
        fonts = *f;
        engine::AppOptions options;
        options.workdir = dir;
        options.pluginPath = {std::filesystem::path(SUPPLE_CLAP_DIR)};  // the stock plugins only
        auto a = engine::EngineApp::create(options);
        REQUIRE(a);
        app = std::move(*a);
        engine::registerEngineCommands(registry);
        dispatcher = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->attachCommands(*dispatcher);
        REQUIRE(gui::generateSession(*dispatcher, app->mediaDir(), 6));
        peaks = std::make_unique<peaks::PeakService>(dir / "cache", [](std::function<void()>) {});
        root = std::make_unique<ui::Root>(*window, fonts, ui::Theme::dark());
        auto arrange = std::make_unique<views::ArrangeView>(
            *peaks,
            [this](const model::MediaSource& src) {
                const std::filesystem::path path(src.path);
                return path.is_absolute() ? path : app->mediaDir() / path;
            },
            views::TransportSource{});
        view = arrange.get();
        root->setContent(std::move(arrange));
        view->installOverlay(*root);
        auto& g = view->gestures();
        g.run = [this](const std::string& label, const std::vector<views::EditStep>& steps) {
            return dispatcher->transact(label, kUi, [&]() -> Result<json::Value> {
                std::vector<json::Value> results;
                for (views::EditStep step : steps) {
                    for (auto& [key, value] : step.args.asObject())
                        if (value.isString() && value.asString().rfind("$step", 0) == 0) {
                            const std::string ref = value.asString().substr(5);
                            const auto dot = ref.find('.');
                            value =
                                *results[std::stoul(ref.substr(0, dot))].find(ref.substr(dot + 1));
                        }
                    auto r = dispatcher->execute({step.command, step.args, kUi});
                    if (!r) return r;
                    results.push_back(*r);
                }
                return results.empty() ? json::Value::object()
                                       : results.back();  // the last step's result
            });
        };
        g.onOpenMidi = [this](model::Id id) { opened.push_back(id); };
        g.onSeek = [this](double s) { seeks.push_back(s); };
        g.onError = [](const Error& e) { MESSAGE("gesture refused: " << e.toString()); };
        views::Viewport v = view->viewport();
        v.pixelsPerSecond = 16;  // the audio items and the MIDI items at bar 21 both visible
        view->setViewport(v);
        sync();
        ok = true;
    }
    ~Fixture() {
        view = nullptr;
        root.reset();
        peaks.reset();
        dispatcher.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    void sync() {
        view->show(app->head());
        root->frame(false);
    }
    std::size_t steps() {
        auto r = dispatcher->execute({"history.list", json::Value::object(), kUi});
        return r->find("versions")->size();
    }
    const model::Item& item(model::Id id) { return *app->head().items.find(id); }
    // The first item on a row whose active take is audio (or MIDI).
    model::Id itemOnRow(std::size_t row, bool midi = false) {
        const model::Session& s = app->head();
        const model::Id track = view->trackLayout().rows()[row].track;
        std::optional<model::Id> best;
        double bestPos = 1e300;
        for (const auto& e : s.items) {
            if (e.value->track != track) continue;
            const model::Take* t =
                e.value->activeTake ? s.takes.find(*e.value->activeTake) : nullptr;
            const bool isMidi = t && t->source && t->source->kind == model::Kind::Midi;
            if (isMidi != midi || e.value->position >= bestPos) continue;
            best = e.id;
            bestPos = e.value->position;
        }
        REQUIRE(best);
        return *best;
    }
    ui::Rect rectOf(model::Id id) {
        const model::Item& it = item(id);
        return view->lanes().itemRect(*view->trackLayout().indexOf(it.track), it);
    }
    float beatPx() {
        const model::Session& s = app->head();
        return static_cast<float>((s.timeMap->secondsAt(1) - s.timeMap->secondsAt(0)) *
                                  view->viewport().pixelsPerSecond);
    }
    void pointer(ui::PointerEvent::Type type, ui::Point p, int button = 1, int clicks = 1,
                 unsigned mods = 0) {
        ui::PointerEvent e;
        e.type = type;
        e.pos = p;
        e.button = button;
        e.clicks = clicks;
        e.mods = mods;
        root->pointer(e);
    }
    void drag(ui::Point from, ui::Point to, unsigned mods = 0, int button = 1) {
        pointer(ui::PointerEvent::Type::Down, from, button, 1, mods);
        pointer(ui::PointerEvent::Type::Move, {(from.x + to.x) / 2, (from.y + to.y) / 2}, button, 1,
                mods);
        pointer(ui::PointerEvent::Type::Move, to, button, 1, mods);
        pointer(ui::PointerEvent::Type::Up, to, button, 1, mods);
        sync();
    }
    void click(ui::Point p, unsigned mods = 0) {
        pointer(ui::PointerEvent::Type::Down, p, 1, 1, mods);
        pointer(ui::PointerEvent::Type::Up, p, 1, 1, mods);
        sync();
    }
};

}  // namespace

TEST_CASE("arrange gestures: select, move (snapped, free, across tracks), trim, slip, fades") {
    Fixture f;
    if (!f.ok) {
        MESSAGE("skipped: no offscreen GL");
        return;
    }
    auto& g = f.view->gestures();
    const model::Id a = f.itemOnRow(0), b = f.itemOnRow(1);
    const ui::Rect ra = f.rectOf(a);
    REQUIRE(ra.w > 40);
    // Click selects; Shift adds; Ctrl toggles.
    f.click({ra.x + ra.w / 2, ra.y + ra.h / 2});
    REQUIRE(g.selection() == std::vector<model::Id>{a});
    const ui::Rect rb = f.rectOf(b);
    f.click({rb.x + rb.w / 2, rb.y + rb.h / 2}, ui::kShift);
    CHECK(g.selection().size() == 2);
    f.click({rb.x + rb.w / 2, rb.y + rb.h / 2}, ui::kCtrl);
    CHECK(g.selection() == std::vector<model::Id>{a});

    // Move one item two beats later: snapped to the grid, one history entry.
    const double grid = g.gridBeats();
    const double posA = f.item(a).position;
    auto s0 = f.steps();
    const ui::Point mid{ra.x + ra.w / 2, ra.y + ra.h / 2};
    f.drag(mid, {mid.x + 2 * f.beatPx() + 1, mid.y});
    CHECK(f.steps() == s0 + 1);
    const double moved = f.item(a).position - posA;
    CHECK(moved > 0);
    CHECK(std::abs(std::round(f.item(a).position / grid) * grid - f.item(a).position) <
          1e-9);  // on the grid

    // Trim the end two grid steps shorter, then the start later (the take's
    // offset follows so the audio stays where it was).
    const ui::Rect rt = f.rectOf(a);
    const double len0 = f.item(a).length;
    const float step = static_cast<float>(grid) * f.beatPx();
    f.drag({rt.right() - 2, rt.y + rt.h / 2}, {rt.right() - 2 - 2 * step, rt.y + rt.h / 2});
    CHECK(f.item(a).length == doctest::Approx(len0 - 2 * grid));
    const ui::Rect rl = f.rectOf(a);
    const model::Take& take0 = *f.app->head().takes.find(*f.item(a).activeTake);
    const double offset0 = take0.startOffset, pos0 = f.item(a).position, len1 = f.item(a).length;
    s0 = f.steps();
    f.drag({rl.x + 2, rl.y + rl.h / 2}, {rl.x + 2 + step, rl.y + rl.h / 2});
    CHECK(f.steps() == s0 + 1);  // item.set and take.set together
    const double d = f.item(a).position - pos0;
    CHECK(d > 0);
    CHECK(f.item(a).length == doctest::Approx(len1 - d));
    const model::Session& sn = f.app->head();
    const double seconds = sn.timeMap->secondsAt(f.item(a).position) - sn.timeMap->secondsAt(pos0);
    CHECK(sn.takes.find(*f.item(a).activeTake)->startOffset == doctest::Approx(offset0 + seconds));

    // Slip: Alt-drag moves the source under the item, not the item (right:
    // earlier material comes into view, the offset shrinks; not snapped).
    const ui::Rect rs = f.rectOf(a);
    const double posS = f.item(a).position;
    const double offS = f.app->head().takes.find(*f.item(a).activeTake)->startOffset;
    REQUIRE(offS > 0.4);
    f.drag({rs.x + rs.w / 2, rs.y + rs.h / 2}, {rs.x + rs.w / 2 + f.beatPx() / 2, rs.y + rs.h / 2},
           ui::kAlt);
    CHECK(f.item(a).position == posS);
    CHECK(f.app->head().takes.find(*f.item(a).activeTake)->startOffset ==
          doctest::Approx(offS - 0.25));

    // Fade in from the top-left handle.
    const ui::Rect rf = f.rectOf(a);
    f.drag({rf.x + 3, rf.y + 3}, {rf.x + 3 + 2 * f.beatPx(), rf.y + 3});
    CHECK(f.item(a).fadeIn.length > 0.3);

    // Ctrl-drag copies the item two grid steps later, on the same track; the
    // original stays (one undo step).
    {
        const std::size_t count = f.app->head().items.size();
        const ui::Rect rc = f.rectOf(a);
        const double posC = f.item(a).position;
        auto s1 = f.steps();
        f.drag({rc.x + rc.w / 2, rc.y + rc.h / 2}, {rc.x + rc.w / 2 + 2 * step, rc.y + rc.h / 2},
               ui::kCtrl);
        CHECK(f.app->head().items.size() == count + 1);
        CHECK(f.item(a).position == posC);
        CHECK(f.steps() == s1 + 1);
    }

    // Down one row (last: row 1 has items of its own over this one) (row 1 is a channel), the item
    // lands on that track.
    const model::Id trackB = f.item(b).track;
    const ui::Rect ra2 = f.rectOf(a);
    const ui::Rect row1 = f.view->rowRect(1);
    f.drag({ra2.x + 14, ra2.y + ra2.h / 2}, {ra2.x + 14, row1.y + row1.h / 2});  // left of the copy
    CHECK(f.item(a).track == trackB);

    // A locked item does not move.
    REQUIRE(f.dispatcher->execute(
        {"item.set", json::parse(R"({"item":")" + b.str() + R"(","locked":true})").value(), kUi}));
    f.sync();
    const ui::Rect rlk = f.rectOf(b);
    const double posB = f.item(b).position;
    f.drag({rlk.x + rlk.w / 2, rlk.y + rlk.h / 2},
           {rlk.x + rlk.w / 2 + 3 * f.beatPx(), rlk.y + rlk.h / 2});
    CHECK(f.item(b).position == posB);
}

TEST_CASE("arrange gestures: marquee, click to seek, take badge, double-click MIDI, file drop") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    // A right-drag marquee over the first two rows selects their items.
    const ui::Rect lanes = f.view->lanesRect();
    const ui::Rect r0 = f.view->rowRect(0), r1 = f.view->rowRect(1);
    f.drag({lanes.x + 4, r0.y + 4}, {lanes.x + 200, r1.bottom() - 4}, 0, 3);
    CHECK(g.selection().size() >= 2);
    // A click on empty space clears the selection and seeks there.
    const model::Id midi = f.itemOnRow(0, true);
    const ui::Rect rm = f.rectOf(midi);
    const ui::Point empty{rm.x - 20, rm.y + rm.h / 2};  // between the audio item and the MIDI item
    REQUIRE_FALSE(g.hit(empty));
    f.click(empty);
    CHECK(g.selection().empty());
    REQUIRE(f.seeks.size() == 1);
    CHECK(f.seeks[0] == doctest::Approx(f.view->timeAt(empty.x)));
    // Double-click on a MIDI item asks to open it.
    f.pointer(ui::PointerEvent::Type::Down, {rm.x + rm.w / 2, rm.y + rm.h / 2}, 1, 2);
    f.pointer(ui::PointerEvent::Type::Up, {rm.x + rm.w / 2, rm.y + rm.h / 2});
    REQUIRE(f.opened.size() == 1);
    CHECK(f.opened[0] == midi);
    // The take badge cycles the active take.
    std::optional<model::Id> twoTakes;
    for (const auto& e : f.app->head().items)
        if (e.value->takes.size() > 1) twoTakes = e.id;
    REQUIRE(twoTakes);
    const model::Id before = *f.item(*twoTakes).activeTake;
    const ui::Rect rt = f.rectOf(*twoTakes);
    auto s0 = f.steps();
    f.click({rt.right() - 16, rt.y + 8});
    CHECK(*f.item(*twoTakes).activeTake != before);
    CHECK(f.steps() == s0 + 1);
    // Files dropped: the first on the channel under the point, the next on a new track.
    audio::AudioData tone;
    tone.channels.assign(2, std::vector<float>(24000, 0.2f));
    for (const char* name : {"x.wav", "y.wav"})
        REQUIRE(
            writeFileAtomic(f.dir / name, std::span<const std::uint8_t>(*audio::encodeWav(tone))));
    const std::size_t items = f.app->head().items.size(), tracks = f.app->head().tracks.size();
    s0 = f.steps();
    REQUIRE(g.drop({f.dir / "x.wav", f.dir / "y.wav"}, {lanes.x + 300, r1.y + r1.h / 2}));
    CHECK(f.app->head().items.size() == items + 2);
    CHECK(f.app->head().tracks.size() == tracks + 1);
    CHECK(f.steps() == s0 + 1);  // one undo step
}

TEST_CASE("automation lanes: shown under their strip, points added, moved and deleted, modes") {
    Fixture f;
    if (!f.ok) return;
    std::vector<std::pair<std::string, json::Value>> committed;
    views::TcpCallbacks cb;
    cb.commit = [&](std::string_view command, json::Value args, std::string) {
        committed.emplace_back(std::string(command), args);
        REQUIRE(f.dispatcher->execute({std::string(command), std::move(args), kUi}));
    };
    f.view->setTcpCallbacks(std::move(cb));
    const model::Id track = f.view->trackLayout().rows()[0].track;
    auto added = f.dispatcher->execute(
        {"env.add",
         json::parse(
             R"({"track":")" + track.str() +
             R"(","param":"volume","points":[{"beat":0,"value":0},{"beat":16,"value":-12}]})")
             .value(),
         kUi});
    REQUIRE(added);
    const model::Id env = *model::Id::parse((*added)["env"].asString());
    f.sync();
    // The lane is the row right under its strip, shorter, with no items on it.
    const auto lane = f.view->trackLayout().indexOfEnv(env);
    REQUIRE(lane);
    CHECK(*lane == *f.view->trackLayout().indexOf(track) + 1);
    CHECK(f.view->trackLayout().heightOf(*lane) < f.view->trackLayout().heightOf(*lane - 1));
    const ui::Rect laneRect = f.view->rowRect(*lane);
    auto& g = f.view->gestures();
    CHECK_FALSE(g.hit({laneRect.x + 30, laneRect.y + laneRect.h / 2}));
    // Its header in the TCP.
    REQUIRE(f.view->tcpPanel().envControl(env, "mode"));
    CHECK(f.view->tcpPanel().envControl(env, "mode")->visible);

    auto points = [&] { return f.app->head().envelopes.find(env)->points; };
    const ui::Rect area = views::Lanes::envArea(laneRect);
    const model::Session& s0 = f.app->head();
    auto xOfBeat = [&](double beat) { return f.view->xAt(f.app->head().timeMap->secondsAt(beat)); };
    (void)s0;
    // A click on the lane adds a point there (snapped to the grid), one undo step.
    auto steps = f.steps();
    f.click({xOfBeat(8) + 1, area.y + area.h * 0.25f});
    REQUIRE(points().size() == 3);
    CHECK(points()[1].pos == doctest::Approx(8));
    CHECK(points()[1].value > 0);  // the upper quarter is above unity
    CHECK(f.steps() == steps + 1);
    // Dragging the last point up raises it.
    const double before = points()[2].value;
    const float yLast =
        area.bottom() - static_cast<float>(views::EnvScale{}.toNorm(before)) * area.h;
    f.drag({xOfBeat(16), yLast}, {xOfBeat(16), yLast - 12});
    REQUIRE(points().size() == 3);
    CHECK(points()[2].value > before);
    CHECK(points()[2].pos == doctest::Approx(16));  // a vertical drag keeps the time
    // Alt+click deletes the first point.
    const float yFirst = area.bottom() - static_cast<float>(views::EnvScale{}.toNorm(0.0)) * area.h;
    f.click({xOfBeat(0) + 1, yFirst}, ui::kAlt);
    CHECK(points().size() == 2);
    // The header's mode button cycles read -> touch; "hide" hides the lane.
    f.root->click(f.view->tcpPanel().envControl(env, "mode")->id());
    CHECK(f.app->head().envelopes.find(env)->mode == model::EnvMode::Touch);
    f.sync();
    f.root->click(f.view->tcpPanel().envControl(env, "hide")->id());
    f.sync();
    CHECK_FALSE(f.app->head().envelopes.find(env)->visible);
    CHECK_FALSE(f.view->trackLayout().indexOfEnv(env));
}

TEST_CASE("arrange gestures: Alt on an item's right edge stretches it (length and play rate)") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id a = f.itemOnRow(0);
    const ui::Rect r = f.rectOf(a);
    const double len0 = f.item(a).length;
    const float step = static_cast<float>(g.gridBeats()) * f.beatPx();
    auto steps = f.steps();
    // Longer by two grid steps: the take plays slower so its material fills it.
    f.drag({r.right() - 2, r.y + r.h / 2}, {r.right() - 2 + 2 * step, r.y + r.h / 2}, ui::kAlt);
    const double len1 = f.item(a).length;
    CHECK(len1 == doctest::Approx(len0 + 2 * g.gridBeats()));
    const model::Take& take = *f.app->head().takes.find(*f.item(a).activeTake);
    CHECK(take.playrate == doctest::Approx(len0 / len1));  // one tempo: beats scale as seconds
    CHECK(f.steps() == steps + 1);
}

TEST_CASE("arrange gestures: a move that overlaps another item crossfades both over the overlap") {
    Fixture f;
    if (!f.ok) return;
    const model::Id audio = f.itemOnRow(0), midi = f.itemOnRow(0, true);
    const double audioEnd = f.item(audio).position + f.item(audio).length;
    REQUIRE(f.item(midi).position > audioEnd);
    // Drag the later item back so it starts 4 beats before the earlier one ends.
    const double target = audioEnd - 4;
    const ui::Rect r = f.rectOf(midi);
    const float dx = static_cast<float>(target - f.item(midi).position) * f.beatPx();
    auto steps = f.steps();
    f.drag({r.x + 20, r.y + r.h / 2}, {r.x + 20 + dx, r.y + r.h / 2});
    REQUIRE(f.item(midi).position == doctest::Approx(target));
    const model::Session& s = f.app->head();
    const double overlap = s.timeMap->secondsAt(audioEnd) - s.timeMap->secondsAt(target);
    CHECK(f.item(audio).fadeOut.length == doctest::Approx(overlap));
    CHECK(f.item(midi).fadeIn.length == doctest::Approx(overlap));
    CHECK(f.steps() == steps + 1);  // the move and both fades: one undo step
    // Off: a move makes no fades.
    f.view->gestures().autoCrossfade = false;
    const ui::Rect r2 = f.rectOf(midi);
    f.drag({r2.x + 20, r2.y + r2.h / 2},
           {r2.x + 20 - 2 * f.beatPx() * static_cast<float>(f.view->gestures().gridBeats()),
            r2.y + r2.h / 2});
    CHECK(f.item(audio).fadeOut.length == doctest::Approx(overlap));  // unchanged
}

TEST_CASE("automation lanes: a plugin parameter's lane uses the plugin's range and curve") {
    Fixture f;
    if (!f.ok) return;
    auto listed = f.dispatcher->execute({"plugin.list", json::Value::object(), kUi});
    REQUIRE(listed);
    // A stock effect with a continuous semantic parameter (partial builds have none).
    std::string fxId, key;
    std::shared_ptr<params::DescribedParam> spec;
    const model::Id track = f.view->trackLayout().rows()[0].track;
    for (const json::Value& p : (*listed)["plugins"].asArray()) {
        if (p.find("instrument") && p.find("instrument")->asBool()) continue;
        auto added =
            f.dispatcher->execute({"fx.add",
                                   json::parse(R"({"track":")" + track.str() + R"(","plugin":")" +
                                               p.find("id")->asString() + R"("})")
                                       .value(),
                                   kUi});
        if (!added) continue;
        const std::string id = (*added)["fx"].asString();
        auto described = f.dispatcher->execute(
            {"fx.describe", json::parse(R"({"fx":")" + id + R"(","full":true})").value(), kUi});
        if (described)
            if (const json::Value* ps = described->find("params"); ps && ps->isObject())
                for (const auto& m : ps->asObject()) {
                    auto d = params::fromDescription(m.key, m.value);
                    if (!d || (*d)->spec.type != params::Type::Float ||
                        ((*d)->spec.flags & (params::kReadonly | params::kHidden)))
                        continue;
                    fxId = id;
                    key = m.key;
                    spec = std::shared_ptr<params::DescribedParam>(std::move(*d));
                    break;
                }
        if (spec) break;
    }
    if (!spec) {
        MESSAGE("skipped: no stock effect with a continuous parameter (partial build)");
        return;
    }
    const double high = params::fromNormalized(spec->spec, 0.9);
    auto added = f.dispatcher->execute(
        {"env.add",
         json::parse(R"({"fx":")" + fxId + R"(","param":")" + key +
                     R"(","points":[{"beat":0,"value":)" + std::to_string(spec->spec.def) +
                     R"(},{"beat":16,"value":)" + std::to_string(high) + "}]}")
             .value(),
         kUi});
    REQUIRE_MESSAGE(added, (added ? std::string() : added.error().toString()));
    const model::Id env = *model::Id::parse((*added)["env"].asString());
    // Without the app's scale source the lane is not shown; with it, it is.
    f.sync();
    CHECK_FALSE(f.view->trackLayout().indexOfEnv(env));
    auto scales = std::make_shared<gui::PluginScales>(*f.dispatcher);
    f.view->trackLayout().pluginScales = [scales](const model::Envelope& e) {
        return (*scales)(e);
    };
    (void)f.dispatcher->execute(
        {"env.set", json::parse(R"({"env":")" + env.str() + R"(","visible":true})").value(), kUi});
    f.sync();
    const auto lane = f.view->trackLayout().indexOfEnv(env);
    REQUIRE(lane);
    CHECK(*lane == *f.view->trackLayout().indexOf(track) + 1);
    // A click in the lane's upper quarter adds a value three quarters up the
    // parameter's own curve.
    const ui::Rect area = views::Lanes::envArea(f.view->rowRect(*lane));
    const float x = f.view->xAt(f.app->head().timeMap->secondsAt(8)) + 1;
    f.click({x, area.y + area.h * 0.25f});
    const auto& pts = f.app->head().envelopes.find(env)->points;
    REQUIRE(pts.size() == 3);
    CHECK(params::toNormalized(spec->spec, pts[1].value) == doctest::Approx(0.75).epsilon(0.05));
}

TEST_CASE(
    "arrange gestures: a double-click on empty channel space makes an empty MIDI item there") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id audio = f.itemOnRow(0), midi = f.itemOnRow(0, true);
    const ui::Rect ra = f.rectOf(audio), rm = f.rectOf(midi);
    const ui::Point empty{(ra.right() + rm.x) / 2, rm.y + rm.h / 2};
    REQUIRE_FALSE(g.hit(empty));
    const std::size_t items = f.app->head().items.size();
    auto steps = f.steps();
    f.pointer(ui::PointerEvent::Type::Down, empty, 1, 2);
    f.pointer(ui::PointerEvent::Type::Up, empty);
    f.sync();
    REQUIRE(f.app->head().items.size() == items + 1);
    REQUIRE(f.opened.size() == 1);
    const model::Item& made = f.item(f.opened[0]);
    const model::Session& s = f.app->head();
    const int bar = s.timeMap->barBeat(s.timeMap->beatAt(f.view->timeAt(empty.x))).bar;
    CHECK(made.position == doctest::Approx(s.timeMap->beatOfBar(bar)));  // from the bar's start
    CHECK(made.length ==
          doctest::Approx(s.timeMap->beatOfBar(bar + 1) - s.timeMap->beatOfBar(bar)));
    const model::Take* take = s.takes.find(*made.activeTake);
    REQUIRE(take);
    REQUIRE(take->source);
    CHECK(take->source->kind == model::Kind::Midi);
    CHECK(s.midi.find(*take->source)->sequence.notes.empty());
    CHECK(g.selection() == std::vector<model::Id>{f.opened[0]});
    CHECK(f.steps() == steps + 1);
    // Over a range: from the start of the bar holding the start to the end.
    auto ranged = g.newMidiItem(f.view->trackLayout().rows()[1].track, 5.0, 13.0);
    REQUIRE(ranged);
    CHECK(f.item(*ranged).position == doctest::Approx(4.0));
    CHECK(f.item(*ranged).length == doctest::Approx(9.0));
}

TEST_CASE("arrange clipboard: copy, cut, paste onto another channel, duplicate") {
    Fixture f;
    if (!f.ok) return;
    auto& g = f.view->gestures();
    const model::Id a = f.itemOnRow(0);
    const double len = f.item(a).length;
    g.setSelection({a});
    // Copy, then paste at beat 200 onto row 1's channel: one undo step, selected.
    g.copySelection();
    REQUIRE(g.canPaste());
    const model::Id row1 = f.view->trackLayout().rows()[1].track;
    auto steps = f.steps();
    REQUIRE(g.paste(200, row1));
    f.sync();
    REQUIRE(g.selection().size() == 1);
    const model::Id pasted = g.selection()[0];
    CHECK(pasted != a);
    CHECK(f.item(pasted).track == row1);
    CHECK(f.item(pasted).position == doctest::Approx(200));
    CHECK(f.item(pasted).length == doctest::Approx(len));
    CHECK(f.steps() == steps + 1);
    // Cut removes it; a paste brings it back as it was (from the copy's version).
    g.setSelection({pasted});
    REQUIRE(g.cutSelection());
    f.sync();
    CHECK_FALSE(f.app->head().items.find(pasted));
    REQUIRE(g.paste(300));
    f.sync();
    REQUIRE(g.selection().size() == 1);
    CHECK(f.item(g.selection()[0]).track == row1);  // its own row (no target given)
    CHECK(f.item(g.selection()[0]).position == doctest::Approx(300));
    // Duplicate: right after itself, on the same track.
    const model::Id last = g.selection()[0];
    REQUIRE(g.duplicateSelection());
    f.sync();
    REQUIRE(g.selection().size() == 1);
    CHECK(f.item(g.selection()[0]).position == doctest::Approx(300 + len));
    CHECK(f.item(g.selection()[0]).track == f.item(last).track);
}

TEST_CASE("take comping: click a take lane to hear it, swipe a range to comp it in") {
    Fixture f;
    if (!f.ok) return;
    std::optional<model::Id> it;
    for (const auto& e : f.app->head().items)
        if (e.value->takes.size() == 2) it = e.id;
    REQUIRE(it);
    const ui::Rect r = f.rectOf(*it);
    REQUIRE(views::Lanes::showsTakeLanes(r, 2));
    const std::size_t heard = static_cast<std::size_t>(
        std::find(f.item(*it).takes.begin(), f.item(*it).takes.end(), *f.item(*it).activeTake) -
        f.item(*it).takes.begin());
    const std::size_t other = 1 - heard;
    auto laneCenter = [&](const ui::Rect& ir, std::size_t lane, float fraction) {
        const ui::Rect l = views::Lanes::takeLaneRect(ir, 2, lane);
        return ui::Point{ir.x + ir.w * fraction, l.y + l.h / 2};
    };
    // A click on the other take's lane: the whole item plays it (one step).
    auto steps = f.steps();
    f.click(laneCenter(r, other, 0.5f));
    CHECK(*f.item(*it).activeTake == f.item(*it).takes[other]);
    CHECK(f.steps() == steps + 1);
    // Swipe the first take's lane across the middle: split twice, the middle
    // piece plays the first take, the outer pieces keep the other.
    const std::size_t items = f.app->head().items.size();
    steps = f.steps();
    const ui::Rect r2 = f.rectOf(*it);
    f.drag(laneCenter(r2, heard, 0.3f), laneCenter(r2, heard, 0.6f));
    CHECK(f.app->head().items.size() == items + 2);
    CHECK(f.steps() == steps + 1);
    const model::Session& s = f.app->head();
    std::vector<const model::Item*> pieces;
    for (const auto& e : s.items)
        if (e.value->track == f.item(*it).track && e.value->takes.size() == 2)
            pieces.push_back(e.value.get());
    std::sort(pieces.begin(), pieces.end(),
              [](auto* a, auto* b) { return a->position < b->position; });
    REQUIRE(pieces.size() == 3);
    CHECK(*pieces[0]->activeTake == pieces[0]->takes[other]);
    CHECK(*pieces[1]->activeTake == pieces[1]->takes[heard]);
    CHECK(*pieces[2]->activeTake == pieces[2]->takes[other]);
    CHECK(pieces[1]->position == doctest::Approx(pieces[0]->position + pieces[0]->length));
}
