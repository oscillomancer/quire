// <slug>: the DAW with a window (docs/specs/gui.md). The UI thread is the
// model thread: it pumps the command bus's loop (socket requests, the service
// tick) between SDL events, and draws only when something changed.
//
//   <slug> [PROJECT] [--offscreen] [--size WxH] [--frames N] [--snapshot PNG at exit]
//          [--bench] [--no-socket] [--instance NAME] [--socket PATH]
//          [--workdir DIR] [--plugin-path P]... [--generate N] [--script]
//          [--idle SECONDS] [--view START_SECONDS,PIXELS_PER_SECOND]
//          [--audio jack|dummy|none] [--theme dark|light|FILE] [--action ID]...
#include <SDL3/SDL.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "actions_list.h"
#include "agent_panel.h"
#include "app_actions.h"
#include "chrome.h"
#include "file_actions.h"
#include "fx_window.h"
#include "generate.h"
#include "history_panel.h"
#include "meter_feed.h"
#include "midi_editor.h"
#include "mixer.h"
#include "plugin_windows.h"
#include "render_dialog.h"
#include "routing_matrix.h"
#include "shell.h"
#include "supple/base/files.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/host/clap_host.h"
#include "supple/json/parse.h"
#include "supple/peaks/peaks.h"
#include "supple/product.h"
#include "supple/rpc/model_loop.h"
#include "supple/rpc/protocol.h"
#include "supple/ui/root.h"
#include "ui_persistence.h"

namespace fs = std::filesystem;
using namespace supple;

namespace {

struct Options {
    fs::path project, workdir, socket, snapshot;
    std::vector<fs::path> pluginPath;
    std::string instance = "default";
    bool offscreen = false, bench = false, serve = true, script = false;
    int width = 1280, height = 800, frames = 0;  // frames 0 = run until closed
    int generate = 0;                            // tracks of a generated session
    double idleSeconds = 0;                      // measure idle CPU after the frames
    double viewStart = -1, viewPps = 0;          // initial lanes viewport
    std::string audio;           // jack | dummy | none; default: jack (none offscreen)
    std::string theme = "dark";  // dark | light | a supple.theme/1.0 file
    // Run in order once the project and its UI state are loaded: an action id,
    // optionally followed by a space and a JSON object of its prompted inputs.
    std::vector<std::string> actions;
};

int usage() {
    std::fprintf(stderr,
                 "usage: %s [PROJECT] [--offscreen] [--size WxH] [--frames N] [--snapshot PNG]\n"
                 "       [--bench] [--no-socket] [--instance NAME] [--socket PATH]\n"
                 "       [--workdir DIR] [--plugin-path P]... [--generate N] [--script]\n"
                 "       [--idle SECONDS] [--view START_SECONDS,PIXELS_PER_SECOND]\n"
                 "       [--audio jack|dummy|none] [--theme dark|light|FILE] [--action ID]...\n",
                 std::string(product::kSlug).c_str());
    return 2;
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--offscreen")
            o.offscreen = true;
        else if (a == "--bench")
            o.bench = true;
        else if (a == "--no-socket")
            o.serve = false;
        else if (a == "--script")
            o.script = true;
        else if (a == "--audio") {
            const char* v = value();
            if (!v ||
                (std::string(v) != "jack" && std::string(v) != "dummy" && std::string(v) != "none"))
                return false;
            o.audio = v;
        } else if (a == "--action") {
            const char* v = value();
            if (!v) return false;
            o.actions.emplace_back(v);
        } else if (a == "--theme") {
            const char* v = value();
            if (!v) return false;
            o.theme = v;
        } else if (a == "--view") {
            const char* v = value();
            if (!v || std::sscanf(v, "%lf,%lf", &o.viewStart, &o.viewPps) != 2) return false;
        } else if (a == "--generate") {
            const char* v = value();
            if (!v) return false;
            o.generate = std::clamp(std::atoi(v), 1, 1024);
        } else if (a == "--idle") {
            const char* v = value();
            if (!v) return false;
            o.idleSeconds = std::max(0.0, std::atof(v));
        } else if (a == "--size") {
            const char* v = value();
            if (!v || std::sscanf(v, "%dx%d", &o.width, &o.height) != 2) return false;
        } else if (a == "--frames") {
            const char* v = value();
            if (!v) return false;
            o.frames = std::max(1, std::atoi(v));
        } else if (a == "--snapshot") {
            const char* v = value();
            if (!v) return false;
            o.snapshot = v;
        } else if (a == "--instance") {
            const char* v = value();
            if (!v) return false;
            o.instance = v;
        } else if (a == "--socket") {
            const char* v = value();
            if (!v) return false;
            o.socket = v;
        } else if (a == "--workdir") {
            const char* v = value();
            if (!v) return false;
            o.workdir = v;
        } else if (a == "--plugin-path") {
            const char* v = value();
            if (!v) return false;
            o.pluginPath.emplace_back(v);
        } else if (!a.empty() && a[0] != '-' && o.project.empty())
            o.project = fs::absolute(a);
        else
            return false;
    }
    return o.width > 0 && o.height > 0;
}

// --theme: a built-in theme, or a supple.theme/1.0 file of overrides on the
// dark theme ("base": "light" starts from the light one).
Result<ui::Theme> loadTheme(const std::string& spec) {
    if (spec == "dark") return ui::Theme::dark();
    if (spec == "light") return ui::Theme::light();
    auto bytes = readFile(spec);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    const json::Value* base = doc->isObject() ? doc->find("base") : nullptr;
    const bool light = base && base->isString() && base->asString() == "light";
    return ui::Theme::fromJson(*doc, light ? ui::Theme::light() : ui::Theme::dark());
}

// Milliseconds since the kernel started this process (exec and dynamic
// loading included), for the cold-start gate (ADR-0005 §5: < 300 ms).
double millisecondsSinceProcessStart() {
    std::ifstream stat("/proc/self/stat");
    std::string line;
    std::getline(stat, line);
    const auto close = line.rfind(')');
    if (close == std::string::npos) return -1;
    std::istringstream fields(line.substr(close + 2));
    std::string field;
    for (int i = 3; i <= 22 && fields >> field; ++i)
        if (i == 22) {
            timespec boot{};
            clock_gettime(CLOCK_BOOTTIME, &boot);
            const double nowS =
                static_cast<double>(boot.tv_sec) + 1e-9 * static_cast<double>(boot.tv_nsec);
            const double startS = std::stod(field) / static_cast<double>(sysconf(_SC_CLK_TCK));
            return 1000.0 * (nowS - startS);
        }
    return -1;
}

double processCpuMs() {
    timespec t{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return 1000.0 * static_cast<double>(t.tv_sec) + 1e-6 * static_cast<double>(t.tv_nsec);
}

double wallMs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return 1000.0 * static_cast<double>(t.tv_sec) + 1e-6 * static_cast<double>(t.tv_nsec);
}

double threadCpuMs() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return 1000.0 * static_cast<double>(t.tv_sec) + 1e-6 * static_cast<double>(t.tv_nsec);
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5)];
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return usage();

    auto platform = gui::Platform::start(o.offscreen ? gui::Platform::Driver::Offscreen
                                                     : gui::Platform::Driver::X11);
    if (!platform) {
        std::fprintf(stderr, "%s\n", platform.error().toString().c_str());
        return 1;
    }
    auto window = gui::Window::open(**platform,
                                    {std::string(product::kName), o.width, o.height, o.offscreen});
    if (!window) {
        std::fprintf(stderr, "%s\n", window.error().toString().c_str());
        return 1;
    }
    auto fonts = gui::loadFonts((*window)->vg(), SUPPLE_GUI_FONTS_DIR);
    if (!fonts) {
        std::fprintf(stderr, "%s\n", fonts.error().toString().c_str());
        return 1;
    }

    engine::AppOptions options;
    options.identity.name = std::string(product::kName);
    options.identity.vendor = std::string(product::kVendor);
    options.identity.version = std::string(product::kVersion);
    // --plugin-path replaces the search path; otherwise the stock plugins built
    // beside this binary come before the standard locations.
    options.pluginPath = o.pluginPath;
    if (options.pluginPath.empty()) {
        if (auto stock = host::stockPluginDir()) options.pluginPath.push_back(*stock);
        for (auto& p : host::defaultSearchPath()) options.pluginPath.push_back(p);
    }
    options.workdir = !o.workdir.empty()   ? o.workdir
                      : !o.project.empty() ? o.project.parent_path()
                                           : fs::current_path();
    auto app = engine::EngineApp::create(options);
    if (!app) {
        std::fprintf(stderr, "%s\n", app.error().toString().c_str());
        return 1;
    }
    if (!o.project.empty() && fs::exists(o.project)) {
        auto opened = (*app)->open(o.project);
        if (!opened) {
            std::fprintf(stderr, "%s\n", opened.error().toString().c_str());
            return 1;
        }
    }
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    gui::UiState uiState;
    gui::registerGuiCommands(registry, uiState);
    cmd::Dispatcher dispatcher(registry, **app);
    (*app)->attachCommands(dispatcher);
    if (o.generate > 0) {
        auto generated = gui::generateSession(dispatcher, (*app)->mediaDir(), o.generate);
        if (!generated) {
            std::fprintf(stderr, "%s\n", generated.error().toString().c_str());
            return 1;
        }
    }

    // The model loop runs inside the SDL loop: its work wakes SDL_WaitEvent.
    rpc::ModelLoop loop;
    loop.setWaker([p = platform->get()] { p->wake(); });
    std::unique_ptr<rpc::Server> server;
    auto publish = [&] {
        for (auto& [topic, payload] : (*app)->takeEvents())
            if (server) server->publish(topic, payload);
    };
    std::string status = "offscreen";
    if (o.serve) {
        server = std::make_unique<rpc::Server>(registry, dispatcher, [&](auto task) {
            loop.post([&, task = std::move(task)] {
                task();
                publish();
            });
        });
        const fs::path path = o.socket.empty() ? rpc::socketPath(o.instance) : o.socket;
        auto started = server->start(path);
        if (!started) {
            std::fprintf(stderr, "%s\n", started.error().toString().c_str());
            return 1;
        }
        status = "agents: " + path.string();
    }
    auto tick = loop.every(std::chrono::milliseconds(100), [&] {
        (*app)->tick();
        dispatcher.flushJobEvents();  // job events and follow-ups from workers
        publish();
    });

    peaks::PeakService peaks(peaks::defaultCacheDir(product::kSlug),
                             [&](std::function<void()> f) { loop.post(std::move(f)); });
    // One meter feed for both views (the TCP and the mixer), filled from what
    // the engine heard since the last poll, once per pass.
    gui::MeterFeed meterFeed([&] {
        std::vector<gui::StripReading> out;
        for (const engine::MeterReading& m : (*app)->meters()) {
            gui::StripReading r;
            r.track = m.track.str();
            for (int ch = 0; ch < 2; ++ch) {
                r.peak[ch] = m.peak[ch];
                r.rms[ch] = m.rms[ch];
            }
            out.push_back(r);
        }
        return out;
    });
    gui::Shell shell(**app, dispatcher, uiState, status, peaks, meterFeed);
    peaks.onReady = [&](const std::string&) { shell.arrange()->lanes().invalidate(); };
    auto theme = loadTheme(o.theme);
    if (!theme) {
        std::fprintf(stderr, "theme: %s\n", theme.error().toString().c_str());
        return 1;
    }
    ui::Root root(**window, *fonts, std::move(*theme));
    bool quit = false;
    std::vector<std::filesystem::path> dropped;  // files of a drop in progress
    ui::Point dropAt{};
    gui::AppActions appActions(registry, dispatcher, **app, uiState,
                               actions::Keymap::defaultFile(product::kSlug));
    appActions.onError = [](const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
    };
    appActions.addUi("ui.actions", "View: actions list",
                     [&] { gui::openActionsList(root, appActions); });
    appActions.addUi("ui.quit", "File: quit", [&] { quit = true; });
    gui::AgentPanel agentPanel(
        root, **app, registry, dispatcher,
        [&](std::function<void()> f) { loop.post(std::move(f)); },
        agent::Config::defaultFile(product::kSlug), SUPPLE_SKILL_DIR);
    appActions.addUi("ui.agent", "View: agent panel", [&] { agentPanel.toggle(); });
    gui::PluginWindows pluginWindows(**platform, **app, [p = platform->get()] { p->wake(); });
    gui::MidiEditor midiEditor(root, appActions, dispatcher, **app, uiState);
    gui::RoutingMatrix routing(root, appActions, dispatcher, **app);
    shell.onRouting = [&routing](const model::Id&, const ui::Rect& anchor) {
        routing.open(anchor);
    };
    gui::Mixer mixer(root, appActions, dispatcher, **app, meterFeed);
    gui::HistoryPanel history(root, appActions, dispatcher, **app);
    gui::FileActions fileActions(root, appActions, dispatcher, **app, uiState, (*window)->sdl(),
                                 [&](std::function<void()> f) { loop.post(std::move(f)); });
    appActions.addUi("ui.fx.editor", "FX: open editor", "fx", [&](const json::Value& v) -> Status {
        const auto id = v.isString() ? model::Id::parse(v.asString()) : std::nullopt;
        if (!id) return Error("gui.editor", "an effect id like fx_3", "/fx");
        return pluginWindows.open(*id);
    });
    gui::FxWindow fxWindow(**app, dispatcher, root, gui::FxWindow::Hooks{[&](model::Id id) {
        return pluginWindows.open(id);
    }},
                           gui::Favourites::defaultFile(product::kSlug));
    appActions.addUi("ui.fx.chain", "FX: chain…", "track",
                     [&](const json::Value& v) -> Status { return fxWindow.openFor(uiState, v); });
    appActions.addUi("ui.fx.add", "FX: add plugin…", "track", [&](const json::Value& v) -> Status {
        return fxWindow.openToAdd(uiState, v);
    });
    gui::RenderDialog renderDialog(**app, dispatcher, root);
    appActions.addUi("ui.render", "File: render…", [&] { renderDialog.open(); });
    root.onUnhandledKey = [&](const ui::KeyEvent& e) {
        const bool typing = root.focused() && root.focused()->role() == ui::Role::TextField;
        return appActions.key(e.key, e.mods, typing);
    };
    auto chromeLayout = gui::ChromeLayout::load(gui::ChromeLayout::defaultFile(product::kSlug));
    if (!chromeLayout)
        std::fprintf(stderr, "menus: %s (using the defaults)\n",
                     chromeLayout.error().toString().c_str());
    {
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->add(gui::makeChrome(root, appActions,
                                   chromeLayout ? *chromeLayout : gui::ChromeLayout::defaults()));
        auto& body = frame->emplace<ui::Node>("main");
        body.layout.dir = ui::Layout::Dir::Row;
        auto& center = body.emplace<ui::Node>("center");
        center.layout.dir = ui::Layout::Dir::Column;
        center.layout.width = ui::SizeRule::fraction();
        center.add(shell.build(root, appActions));
        center.add(midiEditor.build());
        center.add(mixer.build());
        body.add(history.build());
        body.add(agentPanel.build());
        root.setContent(std::move(frame));
    }
    // Arrange editing gestures (P5.6): one transaction per gesture; a later
    // step may use an earlier one's result ("$step0.track").
    {
        views::ItemGestures& g = shell.arrange()->gestures();
        const cmd::Origin ui{cmd::Origin::Type::Ui, "gui", {}};
        g.run = [&dispatcher, ui](
                    const std::string& label,
                    const std::vector<views::EditStep>& steps) -> Result<json::Value> {
            return dispatcher.transact(label, ui, [&]() -> Result<json::Value> {
                std::vector<json::Value> results;
                for (views::EditStep step : steps) {
                    if (step.args.isObject())
                        for (auto& [key, value] : step.args.asObject()) {
                            if (!value.isString() || value.asString().rfind("$step", 0) != 0)
                                continue;
                            const std::string ref = value.asString().substr(5);
                            const auto dot = ref.find('.');
                            const std::size_t at =
                                static_cast<std::size_t>(std::stoul(ref.substr(0, dot)));
                            if (at < results.size())
                                if (const json::Value* v = results[at].find(ref.substr(dot + 1)))
                                    value = *v;
                        }
                    auto r = dispatcher.execute({step.command, step.args, ui});
                    if (!r) return r;
                    results.push_back(std::move(*r));
                }
                return results.empty() ? json::Value::object()
                                       : results.back();  // the last step's result
            });
        };
        g.onSelection = [&](const std::vector<model::Id>& items) {
            uiState.selectedItems.clear();
            for (model::Id id : items) uiState.selectedItems.push_back(id.str());
        };
        g.onOpenMidi = [&](model::Id item) {
            if (auto r = midiEditor.open(item.str()); !r)
                std::fprintf(stderr, "midi: %s\n", r.error().toString().c_str());
        };
        g.onError = [](const Error& e) {
            std::fprintf(stderr, "edit refused: %s\n", e.toString().c_str());
        };
        g.onSeek = [&dispatcher, ui](double seconds) {
            json::Value args = json::Value::object();
            args["seconds"] = seconds;
            (void)dispatcher.execute({"transport.seek", std::move(args), ui});
        };
    }
    shell.arrange()->setRulerCallbacks(
        {[&dispatcher](const std::string& command, json::Value args) {
             return dispatcher.execute(
                 {command, std::move(args), {cmd::Origin::Type::Ui, "gui", {}}});
         },
         [](const Error& e) {
             std::fprintf(stderr, "ruler edit refused: %s\n", e.toString().c_str());
         }});
    // A new, empty MIDI item on the selected channel (else the first), over the
    // loop range when it is on, else one bar at the playhead; opened.
    appActions.addUi("ui.midi.new", "Item: new MIDI item", [&] {
        views::ArrangeView& a = *shell.arrange();
        const model::Session& s = (*app)->head();
        std::optional<model::Id> track;
        for (const auto& t : uiState.selectedTracks)
            if (auto id = model::Id::parse(t);
                id && s.tracks.find(*id) && s.tracks.find(*id)->type == model::StripType::Channel) {
                track = id;
                break;
            }
        for (const auto& row : a.trackLayout().rows())
            if (!track && !row.env && row.type == model::StripType::Channel) track = row.track;
        if (!track) return;
        const bool looping = a.loopEnabled() && a.loopEnd() > a.loopStart();
        const double from = s.timeMap->beatAt(looping ? a.loopStart() : a.playhead());
        const std::optional<double> to =
            looping ? std::optional<double>(s.timeMap->beatAt(a.loopEnd())) : std::nullopt;
        if (auto made = a.gestures().newMidiItem(*track, from, to); !made)
            std::fprintf(stderr, "new MIDI item: %s\n", made.error().toString().c_str());
    });
    // The item clipboard: paste at the playhead, onto the selected channel.
    auto reportEdit = [](const char* what, const Status& st) {
        if (!st) std::fprintf(stderr, "%s: %s\n", what, st.error().toString().c_str());
    };
    appActions.addUi("ui.item.copy", "Item: copy",
                     [&] { shell.arrange()->gestures().copySelection(); });
    appActions.addUi("ui.item.cut", "Item: cut",
                     [&] { reportEdit("cut", shell.arrange()->gestures().cutSelection()); });
    appActions.addUi("ui.item.paste", "Item: paste at the playhead", [&] {
        views::ArrangeView& a = *shell.arrange();
        std::optional<model::Id> track;
        if (!uiState.selectedTracks.empty())
            track = model::Id::parse(uiState.selectedTracks.front());
        reportEdit("paste",
                   a.gestures().paste((*app)->head().timeMap->beatAt(a.playhead()), track));
    });
    appActions.addUi("ui.item.duplicate", "Item: duplicate after itself", [&] {
        reportEdit("duplicate", shell.arrange()->gestures().duplicateSelection());
    });
    // Navigation: the previous/next marker or region start from the playhead.
    auto jump = [&](bool forward) {
        views::ArrangeView& a = *shell.arrange();
        const model::Session& s = (*app)->head();
        auto secondsOf = [&](double pos, model::Timebase tb) {
            return tb == model::Timebase::Time ? pos : s.timeMap->secondsAt(pos);
        };
        std::vector<double> stops;
        for (const auto& m : s.markers) stops.push_back(secondsOf(m.value->pos, m.value->timebase));
        for (const auto& r : s.regions)
            stops.push_back(secondsOf(r.value->start, r.value->timebase));
        const double now = a.playhead();
        std::optional<double> best;
        for (double t : stops)
            if (forward ? t > now + 1e-6 : t < now - 1e-6)
                if (!best || (forward ? t < *best : t > *best)) best = t;
        if (!best) return;
        json::Value args = json::Value::object();
        args["seconds"] = *best;
        if (!dispatcher.execute(
                {"transport.seek", std::move(args), {cmd::Origin::Type::Ui, "gui", {}}}))
            a.setPlayhead(*best);  // the engine is stopped: move the shown playhead
    };
    appActions.addUi("ui.marker.prev", "Marker: go to the previous one", [jump] { jump(false); });
    appActions.addUi("ui.marker.next", "Marker: go to the next one", [jump] { jump(true); });
    appActions.addUi("ui.item.select_all", "Item: select all", [&] {
        auto& g = shell.arrange()->gestures();
        std::vector<model::Id> all;
        for (const auto& e : (*app)->head().items) all.push_back(e.id);
        g.setSelection(all);
        uiState.selectedItems.clear();
        for (model::Id id : all) uiState.selectedItems.push_back(id.str());
    });
    appActions.addUi("ui.marker.add", "Marker: add at playhead",
                     [&] { shell.arrange()->addMarker(); });
    appActions.addUi("ui.region.add", "Region: add over loop range",
                     [&] { shell.arrange()->addRegion(); });
    // The audio engine: the device for interactive use, nothing offscreen.
    const std::string audioMode =
        !o.audio.empty() ? o.audio : (o.offscreen || o.frames > 0 ? "none" : "jack");
    if (audioMode != "none") {
        json::Value args = json::Value::object();
        args["backend"] = audioMode;
        if (auto started = dispatcher.execute(
                {"engine.start", std::move(args), {cmd::Origin::Type::Ui, "gui", {}}});
            !started)
            std::fprintf(
                stderr,
                "audio: %s (run engine.start from the actions list when the device is ready)\n",
                started.error().toString().c_str());
    }

    // Per-project UI state (gui.md §1.2).
    gui::UiPersistence uiFile;
    uiFile.add(
        "selection",
        [&] {
            json::Value v = json::Value::object(), tracks = json::Value::array(),
                        items = json::Value::array();
            for (const auto& t : uiState.selectedTracks) tracks.push_back(t);
            for (const auto& i : uiState.selectedItems) items.push_back(i);
            v["tracks"] = std::move(tracks);
            v["items"] = std::move(items);
            return v;
        },
        [&](const json::Value& v) {
            auto ids = [](const json::Value* a) {
                std::vector<std::string> out;
                if (a && a->isArray())
                    for (const auto& x : a->asArray())
                        if (x.isString()) out.push_back(x.asString());
                return out;
            };
            uiState.selectedTracks = ids(v.find("tracks"));
            uiState.selectedItems = ids(v.find("items"));
        });
    uiFile.add(
        "panels",
        [&] {
            json::Value v = json::Value::object();
            v["agent"] = agentPanel.visible();
            v["mixer"] = mixer.visible();
            v["history"] = history.visible();
            return v;
        },
        [&](const json::Value& v) {
            if (const json::Value* a = v.find("agent"); a && a->isBool())
                agentPanel.setVisible(a->asBool());
            if (const json::Value* m = v.find("mixer"); m && m->isBool())
                mixer.setVisible(m->asBool());
            if (const json::Value* h = v.find("history"); h && h->isBool())
                history.setVisible(h->asBool());
        });
    uiFile.add(
        "window",
        [&] {
            json::Value v = json::Value::object();
            v["width"] = static_cast<int>((*window)->width());
            v["height"] = static_cast<int>((*window)->height());
            return v;
        },
        [&](const json::Value& v) {
            const json::Value* w = v.find("width");
            const json::Value* h = v.find("height");
            if (!o.offscreen && w && h && w->isNumber() && h->isNumber())
                (*window)->setSize(static_cast<int>(w->asInt()), static_cast<int>(h->asInt()));
        });
    // The arrange view's state (zoom, scroll, row heights, follow).
    uiFile.add(
        "arrange",
        [&] {
            const views::ArrangeView& a = *shell.arrange();
            const views::Viewport& vp = a.viewport();
            json::Value v = json::Value::object();
            v["start_seconds"] = vp.startSeconds;
            v["pixels_per_second"] = vp.pixelsPerSecond;
            v["scroll_y"] = static_cast<double>(vp.scrollY);
            v["row_height"] = static_cast<double>(a.trackLayout().globalHeight());
            v["follow"] = std::string(views::followName(a.follow()));
            json::Value heights = json::Value::object();
            for (const auto& [track, h] : a.trackLayout().heights())
                heights[track.str()] = static_cast<double>(h);
            v["track_heights"] = std::move(heights);
            return v;
        },
        [&](const json::Value& v) {
            views::ArrangeView& a = *shell.arrange();
            views::Viewport vp = a.viewport();
            auto num = [&](const char* k, auto& out) {
                if (const json::Value* x = v.find(k); x && x->isNumber())
                    out = static_cast<std::remove_reference_t<decltype(out)>>(x->asDouble());
            };
            num("start_seconds", vp.startSeconds);
            num("pixels_per_second", vp.pixelsPerSecond);
            num("scroll_y", vp.scrollY);
            a.setViewport(vp);
            if (const json::Value* rh = v.find("row_height"); rh && rh->isNumber())
                a.trackLayout().setGlobalHeight(static_cast<float>(rh->asDouble()));
            if (const json::Value* th = v.find("track_heights"); th && th->isObject())
                for (const auto& m : th->asObject()) {
                    const auto id = model::Id::parse(m.key);
                    if (id && m.value.isNumber())
                        a.trackLayout().setTrackHeight(*id, static_cast<float>(m.value.asDouble()));
                }
            if (const json::Value* f = v.find("follow"); f && f->isString())
                if (const auto mode = views::followFrom(f->asString())) a.setFollow(*mode);
        });
    shell.installOverlay(root);  // the playhead paints over the whole window
    if (o.viewStart >= 0) {
        views::Viewport v;
        v.startSeconds = o.viewStart;
        v.pixelsPerSecond = o.viewPps;
        shell.arrange()->setViewport(v);
    }
    int drawn = 0, longTasks = 0;
    double coldStartMs = -1, longestTaskMs = 0;
    std::vector<double> frameCpu;
    // Counted runs (--frames) first wait, uncounted, until every visible
    // pyramid is ready; --script then moves the viewport every frame.
    bool warm = o.frames == 0;
    const double warmUntil = wallMs() + 60000;
    int step = 0;
    while (!quit && !loop.stopRequested()) {
        const double t0 = wallMs();
        int next = loop.pump();
        if (const int plugin = pluginWindows.service(); plugin >= 0)
            next = next < 0 ? plugin : std::min(next, plugin);
        if (const double ms = wallMs() - t0; ms > 2.0) {
            ++longTasks;
            longestTaskMs = std::max(longestTaskMs, ms);
        }
        shell.sync();
        midiEditor.sync();
        routing.sync();
        mixer.sync();
        history.sync();
        meterFeed.poll();
        fxWindow.tick();
        renderDialog.tick();
        uiFile.tick((*app)->projectPath());
        // --action: after the first tick has read the project's UI state.
        for (const auto& spec : std::exchange(o.actions, {})) {
            const auto space = spec.find(' ');
            const std::string id = spec.substr(0, space);
            const actions::Action* action = appActions.registry().find(id);
            Result<json::Value> inputs = json::Value::object();
            if (space != std::string::npos) inputs = json::parse(spec.substr(space + 1));
            Result<json::Value> r =
                !action   ? Result<json::Value>(Error("action.unknown", "no action " + id))
                : !inputs ? Result<json::Value>(inputs.error())
                          : appActions.run(*action, *inputs);
            if (!r) {
                std::fprintf(stderr, "action %s: %s\n", id.c_str(), r.error().toString().c_str());
                return 1;
            }
        }
        const bool moved = shell.arrange()->poll();
        if (moved) root.requestAnimationFrame();
        if (o.script && warm) {
            views::ArrangeView& arrange = *shell.arrange();
            views::Viewport v = arrange.viewport();
            const double k = step++;
            v.pixelsPerSecond = 40.0 * std::pow(2.0, 3.0 * std::sin(k / 40.0));
            v.startSeconds = 2.0 + 20.0 * (1 + std::sin(k / 70.0));
            v.scrollY =
                static_cast<float>((1 + std::sin(k / 25.0)) / 2) *
                std::max(0.0f, arrange.trackLayout().contentHeight() - arrange.lanesRect().h);
            arrange.setViewport(v);
        }
        if (o.frames > 0) root.requestAnimationFrame();  // benchmark runs draw every pass
        if (root.wantsFrame()) {
            const double cpu0 = threadCpuMs();
            root.frame();
            const double cpu = threadCpuMs() - cpu0;
            if (drawn == 0 && coldStartMs < 0) coldStartMs = millisecondsSinceProcessStart();
            if (!warm) {
                warm = shell.arrange()->lanes().stats().pending == 0;
                if (!warm && wallMs() > warmUntil) {
                    std::fprintf(stderr, "peaks did not become ready within 60 s\n");
                    return 1;
                }
            } else {
                frameCpu.push_back(cpu);
                if (o.frames > 0 && ++drawn >= o.frames) break;
                if (o.frames == 0) ++drawn;
            }
        }
        // Selection changed by actions (or the history) reaches the gesture layer.
        if (auto& g = shell.arrange()->gestures();
            g.selection().size() != uiState.selectedItems.size()) {
            std::vector<model::Id> ids;
            for (const auto& s : uiState.selectedItems)
                if (auto id = model::Id::parse(s)) ids.push_back(*id);
            g.setSelection(std::move(ids));
        }
        SDL_Event e;
        int wait = next;
        if (o.frames == 0 && shell.arrange()->playing() && (wait < 0 || wait > 16)) wait = 16;
        const bool got = o.frames > 0 ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, wait);
        for (bool have = got; have; have = SDL_PollEvent(&e)) {
            if (pluginWindows.handle(e)) continue;  // an editor window's event
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (o.frames > 0 || !fileActions.dirty())
                    quit = true;
                else if (!root.popupOpen())
                    fileActions.confirmDiscard("Quit?", [&] { quit = true; });
            } else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                dropped.emplace_back(e.drop.data);
                dropAt = {e.drop.x, e.drop.y};
            } else if (e.type == SDL_EVENT_DROP_COMPLETE) {
                if (auto r = shell.arrange()->gestures().drop(dropped, dropAt); !r)
                    std::fprintf(stderr, "drop: %s\n", r.error().toString().c_str());
                dropped.clear();
            } else
                root.handle(e);  // the wake event is ignored: it only interrupts the wait
        }
    }
    double idleCpuPercent = -1;
    if (o.idleSeconds > 0) {  // no input, no model changes: the loop should sleep
        const double c0 = processCpuMs(), w0 = wallMs();
        while (wallMs() - w0 < o.idleSeconds * 1000) {
            const int next = loop.pump();
            shell.sync();
            midiEditor.sync();
            routing.sync();
            mixer.sync();
            history.sync();
            meterFeed.poll();
            if (root.wantsFrame()) root.frame();
            SDL_Event e;
            const double left = o.idleSeconds * 1000 - (wallMs() - w0);
            const int wait =
                next < 0 ? static_cast<int>(left) : std::min(next, static_cast<int>(left));
            if (SDL_WaitEventTimeout(&e, std::max(1, wait)))
                while (SDL_PollEvent(&e)) {
                }
        }
        idleCpuPercent = 100.0 * (processCpuMs() - c0) / (wallMs() - w0);
    }
    tick.cancel();
    if (server) server->stop();
    (void)uiFile.flush((*app)->projectPath());
    if (!o.snapshot.empty()) {  // the final state, for tests and bug reports
        shell.sync();
        midiEditor.sync();
        routing.sync();
        mixer.sync();
        history.sync();
        meterFeed.poll();
        root.damageAll();
        root.frame(false);
        auto written = gui::writePng(o.snapshot, root.readBack());
        if (!written) {
            std::fprintf(stderr, "%s\n", written.error().toString().c_str());
            return 1;
        }
    }
    if (o.bench) {
        const auto& ls = shell.arrange()->lanes().stats();
        std::printf(
            "{\"schema\": \"supple.gui-bench/1.1\", \"driver\": \"%s\", \"cold_start_ms\": %.1f, "
            "\"frames\": %d, \"frame_cpu_ms\": {\"p50\": %.3f, \"p95\": %.3f, \"max\": %.3f}, "
            "\"ui_tasks_over_2ms\": %d, \"longest_ui_task_ms\": %.2f, \"idle_cpu_percent\": %.2f, "
            "\"tracks\": %d, \"last_frame\": {\"rows\": %zu, \"items\": %zu, \"columns\": %zu, "
            "\"notes\": %zu}, \"peaks\": {\"built\": %d, \"loaded\": %d}, \"gl\": \"%s\"}\n",
            o.offscreen ? "offscreen" : "x11", coldStartMs, drawn, percentile(frameCpu, 0.5),
            percentile(frameCpu, 0.95),
            frameCpu.empty() ? 0.0 : *std::max_element(frameCpu.begin(), frameCpu.end()), longTasks,
            longestTaskMs, idleCpuPercent, o.generate, ls.rows, ls.items, ls.columns, ls.notes,
            peaks.stats().built, peaks.stats().loaded, (*window)->renderer().c_str());
    }
    return 0;
}
