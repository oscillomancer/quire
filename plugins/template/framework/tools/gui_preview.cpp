// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Renders the real editor offscreen and writes a PNG, for reviewing layout
// and look without a host -- by a person or by an agent that can read
// images. Transient states can be staged: a hovered or focused control, an
// open menu, an open text field, values set by name. Each must take: a
// control that is not there, is hidden or disabled, or opens no menu for
// --menu or --context, is an error (exit 2), not a picture of something
// else. A click staged on a control reaches the DSP, as an edit would.
//
//   gui-preview out.png
//   gui-preview out.png --scale 2 --set "Low Cut Enabled=On" --hover "Low Cut"
//   gui-preview menu.png --menu "Mode"              a dropdown's menu
//   gui-preview presets.png --preset "Wide Pan" --menu Presets
//   gui-preview text.png --property "Label=Strings" --text "Label"
//   gui-preview context.png --context "Depth"
//   gui-preview track.png --track "Drum Bus" --track-colour E5484D
//   gui-preview tip.png --tooltip "Low Cut"
//   gui-preview page.png --page 1 --play 60 --drop
//
// Named scenarios: product/previews.txt names the states worth looking at,
// one per line -- a name, then the options above -- so anyone (and CI) can
// render the same ones:
//
//   gui-preview menu.png --scenario context-menu
//   gui-preview --all previews/ [--scale 2]    every scenario, as previews/<name>.png
//   gui-preview --list
//
// "default", the editor as it opens, is always among them.
//
// This proves drawing only. It cannot validate embedding, host timers,
// resizing in a host or teardown; clap-probe and x11-probe do that.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/Engine.h"
#include "core/ParamTable.h"
#include "core/Preset.h"
#include "core/PresetLibrary.h"
#include "core/PropertyTable.h"
#include "gui/Canvas.h"
#include "gui/Gui.h"
#include "ProductInfo.h"

using namespace supple;
using namespace supple::gui;

namespace {

class PreviewHost : public EditorHost, public StateAccess {
  public:
    PreviewHost() {
        for (size_t i = 0; i < params::kCount; ++i) values_[i] = hostDefault(params::at(i));
        // A folder with nothing in it: the factory presets, and saving on.
        if (product::info::kPresets)
            presets_ = std::make_unique<PresetLibrary>(
                *this, std::filesystem::temp_directory_path() / "supple-gui-preview-no-presets");
    }
    std::string_view property(uint32_t id) const override {
        const int i = properties::indexOf(id);
        return i >= 0 ? std::string_view(properties_.get(static_cast<size_t>(i))) : std::string_view();
    }
    bool setProperty(uint32_t id, std::string_view value) override {
        const int i = properties::indexOf(id);
        if (i < 0 || !properties_.set(static_cast<size_t>(i), value)) return false;
        if (engine_) engine_->setProperty(static_cast<size_t>(i), value, true);
        return true;
    }
    uint64_t propertiesVersion() const override { return properties_.version(); }
    FileStatus fileStatus(uint32_t id) const override {
        const int i = properties::indexOf(id);
        if (i < 0) return {};
        if (engine_) return engine_->fileStatus(static_cast<size_t>(i));
        FileStatus s;  // before any audio: what the path says
        s.path = properties_.get(static_cast<size_t>(i));
        s.state = s.path.empty() ? FileStatus::State::Empty
                  : files::exists(s.path) ? FileStatus::State::Loading
                                          : FileStatus::State::Missing;
        return s;
    }
    PresetLibrary* presetLibrary() override { return presets_.get(); }

    // The track, as a host would say it.
    TrackInfo trackInfo;
    TrackInfo track() const override { return trackInfo; }
    // A context menu's host items, as a CLAP host might offer them.
    bool hostMenu(uint32_t, HostMenu& menu) override {
        menu.items = {MenuItem::item("Show Automation"), MenuItem::header("Modulation"), MenuItem::item("Modulate"),
                      MenuItem::item("MIDI Learn", true)};
        menu.actions = {1, 0, 2, 3};
        return true;
    }

    std::vector<uint8_t> saveState(state::Context context) override {
        state::Snapshot s;
        for (size_t i = 0; i < params::kCount; ++i) s.host[i] = values_[i];
        for (size_t i = 0; i < properties::kCount; ++i) {
            s.property[i] = properties_.get(i);
            s.hasProperty[i] = true;
        }
        return state::save(state::fourcc(product::info::kStateTag), s, context);
    }
    bool loadState(const uint8_t* data, size_t size, state::Context context) override {
        state::Snapshot s;
        if (state::load(data, size, state::fourcc(product::info::kStateTag), s) != state::LoadResult::Ok) return false;
        for (size_t i = 0; i < params::kCount; ++i) {
            if (params::at(i).isOutput()) continue;
            values_[i] = s.host[i];
            if (engine_) engine_->store().set(i, s.host[i]);
        }
        for (size_t i = 0; i < properties::kCount; ++i)
            if (context == state::Context::Project || !properties::at(i).perProjectOnly())
                setProperty(properties::at(i).id, s.hasProperty[i] ? std::string_view(s.property[i])
                                                                  : std::string_view(properties::at(i).def));
        return true;
    }
    // An output shows what the DSP reported for the programme fed so far.
    double value(uint32_t id) const override {
        const size_t i = slot(id);
        return engine_ && params::at(i).isOutput() ? engine_->store().get(i) : values_[i];
    }
    void beginEdit(uint32_t) override {}
    // A click reaches the DSP, as a host's edit would.
    void setValue(uint32_t id, double v) override {
        const size_t i = slot(id);
        values_[i] = sanitizeHost(*params::find(id), v);
        if (engine_) engine_->store().set(i, values_[i]);
    }
    void endEdit(uint32_t) override {}
    bool popTelemetry(TelemetryFrame& f) override { return ring_.pop(f); }
    size_t readAudio(FeedFrame* out, size_t max) override {
        AudioFeed* feed = engine_ ? engine_->feed() : nullptr;
        return feed ? feed->read(out, max) : 0;
    }
    void playNote(int key, double velocity, bool on) override {
        if (engine_) engine_->playNote(key, velocity, on);
    }
    int latencySamples() const override { return engine_ ? engine_->latency() : 0; }
    double sampleRate() const override { return kRate; }

    void set(size_t i, double host) { values_[i] = host; }

    // Program-like material through the real engine, so everything the
    // editor shows -- the framework's meters and the product's own telemetry
    // -- is what the DSP makes of it: a slow bed around -20 dBFS with a
    // transient twice a second, three partials, the right channel a little
    // quieter. A sidechain hears a kick on each transient; a product that
    // takes notes is played a C major chord, held.
    void feed(int frames) {
        constexpr int width = product::info::kAudioOutput ? 2 : 0;
        if (!engine_) {
            engine_ = std::make_unique<Engine>();
            for (size_t i = 0; i < params::kCount; ++i) engine_->store().set(i, values_[i]);
            for (size_t i = 0; i < properties::kCount; ++i) engine_->setProperty(i, properties_.get(i), true);
            engine_->activate(kRate, kPeriodMax, product::info::kAudioInput ? width : 0, width);
            engine_->setMetering(true);
        } else if (engine_->takeRestartRequest()) {  // a restartOnChange() parameter moved: as a host restarts
            engine_->deactivate();
            engine_->activate(kRate, kPeriodMax, product::info::kAudioInput ? width : 0, width);
        }
        const int period = std::min(kPeriodMax, static_cast<int>(std::lround(kRate / kTelemetryHz)));
        std::vector<double> l(static_cast<size_t>(period)), r(static_cast<size_t>(period));
        std::vector<double> kl(static_cast<size_t>(period)), kr(static_cast<size_t>(period));
        std::vector<std::vector<double>> aux(static_cast<size_t>(product::info::kAuxOutputs) * 2,
                                             std::vector<double>(static_cast<size_t>(period)));
        double* io[] = {l.data(), r.data()};
        const double* key[] = {kl.data(), kr.data()};
        std::vector<double*> auxChannels;
        for (auto& c : aux) auxChannels.push_back(c.data());
        std::vector<double* const*> auxBuses;
        for (size_t b = 0; b < static_cast<size_t>(product::info::kAuxOutputs); ++b) auxBuses.push_back(auxChannels.data() + 2 * b);
        constexpr double kTwoPi = 6.283185307179586;
        for (int f = 0; f < frames; ++f) {
            for (int k = 0; k < period; ++k) {
                const double t = time_ + k / kRate;
                const double a = std::pow(10.0, envelopeDb(t) / 20.0);
                const double x = a * (0.6 * std::sin(kTwoPi * 110.0 * t) + 0.3 * std::sin(kTwoPi * 440.0 * t) +
                                      0.1 * std::sin(kTwoPi * 1760.0 * t));
                l[static_cast<size_t>(k)] = x;
                r[static_cast<size_t>(k)] = 0.84 * x;
                const double beat = t * 2.0 - std::floor(t * 2.0);
                kl[static_cast<size_t>(k)] = kr[static_cast<size_t>(k)] = 0.8 * std::exp(-beat * 9.0) * std::sin(kTwoPi * 55.0 * beat);
            }
            time_ += period / kRate;
            struct Chord {
                int sent = 0;
                bool next(EngineEvent& e) {
                    if (sent >= 3) return false;
                    e = EngineEvent{};
                    e.type = EngineEvent::Type::Note;
                    e.note.key = static_cast<int16_t>(60 + (sent == 1 ? 4 : sent == 2 ? 7 : 0));
                    e.note.noteId = sent++;
                    e.note.velocity = 0.8;
                    return true;
                }
            } chord;
            if (!product::info::kNoteInput || played_) chord.sent = 3;
            played_ = true;
            AudioBuses<double> buses;
            buses.in = product::info::kAudioInput ? io : nullptr;
            buses.out = width ? io : nullptr;
            buses.sidechain = product::info::kSidechain ? key : nullptr;
            buses.aux = auxBuses.empty() ? nullptr : auxBuses.data();
            engine_->process(buses, period, chord);
            TelemetryFrame frame;
            while (engine_->telemetry().pop(frame)) ring_.push(frame);
        }
    }

  private:
    static constexpr double kRate = 48000.0;
    static constexpr int kPeriodMax = 2048;

    static double envelopeDb(double t) {
        const double beat = t * 2.0 - std::floor(t * 2.0);
        return -20.0 + 3.0 * std::sin(t * 0.7) + 15.0 * std::exp(-beat * 14.0);
    }
    static size_t slot(uint32_t id) {
        const int i = params::indexOf(id);
        return i >= 0 ? static_cast<size_t>(i) : 0;
    }
    double values_[params::kCount];
    TelemetryRing ring_;
    std::unique_ptr<Engine> engine_;
    PropertyStore properties_;
    std::unique_ptr<PresetLibrary> presets_;
    double time_ = 0.0;
    bool played_ = false;
};

int paramByName(const char* name) {
    for (size_t i = 0; i < params::kCount; ++i)
        if (!std::strcmp(params::at(i).name, name)) return static_cast<int>(i);
    return -1;
}

int propertyByName(const char* name) {
    for (size_t i = 0; i < properties::kCount; ++i)
        if (!std::strcmp(properties::at(i).name, name)) return static_cast<int>(i);
    return -1;
}

// --- a PNG writer: stored deflate blocks, no dependency -----------------------------

uint32_t crc(const uint8_t* data, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

void be32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    be32(out, crc(body.data(), body.size()) ^ 0xFFFFFFFFu);
}

bool writePng(const char* path, int w, int h, const std::vector<unsigned char>& rgbBottomUp) {
    std::vector<uint8_t> raw;
    for (int y = h - 1; y >= 0; --y) {  // GL rows run bottom to top
        raw.push_back(0);
        const unsigned char* row = rgbBottomUp.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (size_t at = 0; at < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        for (size_t i = 0; i < n; ++i) {
            a = (a + raw[at + i]) % 65521;
            b = (b + a) % 65521;
        }
        z.insert(z.end(), raw.begin() + static_cast<long>(at), raw.begin() + static_cast<long>(at + n));
        at += n;
    }
    be32(z, (b << 16) | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> header;
    be32(header, static_cast<uint32_t>(w));
    be32(header, static_cast<uint32_t>(h));
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

struct Capture {
    bool want = false, got = false, seen = false;
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
};

void onFrame(void* user) {
    auto* c = static_cast<Capture*>(user);
    c->seen = true;
    if (!c->want || c->got) return;
    canvasViewportSize(&c->w, &c->h);
    if (c->w <= 0 || c->h <= 0) return;
    c->rgb.resize(static_cast<size_t>(c->w) * static_cast<size_t>(c->h) * 3);
    canvasReadPixels(c->w, c->h, c->rgb.data());
    c->got = true;
}

void usage() {
    std::printf(
        "gui-preview <out.png> [--scale N] [--set \"Name=value\"]... [--property \"Name=text\"]...\n"
        "            [--preset NAME] [--hover NAME] [--focus NAME] [--menu NAME|Presets] [--text NAME]\n"
        "            [--context NAME] [--track NAME] [--track-colour RRGGBB] [--tooltip NAME]\n"
        "            [--page N] [--play KEY] [--drop] [--seconds S] [--scenario NAME]\n"
        "gui-preview --all <folder> [options]    every scenario in product/previews.txt\n"
        "gui-preview --list\n");
}

// product/previews.txt: "name option option ...", options split as a shell
// would at spaces, with double quotes around one that has spaces in it.
struct Scenario {
    std::string name;
    std::vector<std::string> args;
};

// The product's, or (for gui-preview's own tests) the file the environment
// names.
const char* previewsFile() {
    const char* file = std::getenv("SUPPLE_PREVIEWS_FILE");
    return file && *file ? file : SUPPLE_PREVIEWS_FILE;
}

std::vector<Scenario> scenarios() {
    std::vector<Scenario> out{{"default", {}}};
    std::ifstream f(previewsFile());
    for (std::string line; std::getline(f, line);) {
        std::vector<std::string> words;
        std::string word;
        bool quoted = false, any = false;
        for (char c : line) {
            if (c == '"') {
                quoted = !quoted;
                any = true;
            } else if (!quoted && (c == ' ' || c == '\t' || c == '\r')) {
                if (any) words.push_back(word);
                word.clear();
                any = false;
            } else {
                word += c;
                any = true;
            }
        }
        if (any) words.push_back(word);
        if (words.empty() || words[0].empty() || words[0][0] == '#') continue;
        Scenario s{words[0], {words.begin() + 1, words.end()}};
        if (s.name == "default") out[0] = s;
        else out.push_back(s);
    }
    return out;
}

const Scenario* findScenario(const std::vector<Scenario>& all, const std::string& name) {
    for (const Scenario& s : all)
        if (s.name == name) return &s;
    return nullptr;
}

// One preview: the editor in the state the options stage, written as a PNG.
int preview(const char* output, std::vector<std::string> args) {
    // A scenario's options, in place of its name.
    for (size_t i = 0, expanded = 0; i + 1 < args.size(); ++i) {
        if (args[i] != "--scenario") continue;
        const std::vector<Scenario> all = scenarios();
        const Scenario* s = findScenario(all, args[i + 1]);
        if (!s) {
            std::fprintf(stderr, "no scenario \"%s\" in %s\n", args[i + 1].c_str(), previewsFile());
            return 2;
        }
        if (++expanded > 100) {
            std::fprintf(stderr, "scenario \"%s\" leads back to itself\n", args[i + 1].c_str());
            return 2;
        }
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i + 2));
        args.insert(args.begin() + static_cast<std::ptrdiff_t>(i), s->args.begin(), s->args.end());
        i = static_cast<size_t>(-1);
    }
    const int argc = static_cast<int>(args.size()) + 2;
    std::vector<const char*> argv{"gui-preview", output};
    for (const std::string& a : args) argv.push_back(a.c_str());
    double scale = 1.0, seconds = 3.0;
    const char *hover = nullptr, *focus = nullptr, *menu = nullptr, *text = nullptr, *context = nullptr,
               *tooltip = nullptr;
    int page = -1, play = -1;
    bool drop = false;
    PreviewHost host;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[static_cast<size_t>(i)];
        if (arg == "--drop") {  // the one option without a value
            drop = true;
            continue;
        }
        const char* next = i + 1 < argc ? argv[static_cast<size_t>(i) + 1] : nullptr;
        if (!next) return usage(), 2;
        ++i;
        if (arg == "--scale") scale = std::atof(next);
        else if (arg == "--seconds") seconds = std::atof(next);
        else if (arg == "--hover") hover = next;
        else if (arg == "--focus") focus = next;
        else if (arg == "--menu") menu = next;
        else if (arg == "--text") text = next;
        else if (arg == "--context") context = next;
        else if (arg == "--tooltip") tooltip = next;
        else if (arg == "--page") page = std::atoi(next);
        else if (arg == "--play") play = std::atoi(next);
        else if (arg == "--track") host.trackInfo.name = next;
        else if (arg == "--track-colour") {
            const unsigned long rgb = std::strtoul(next, nullptr, 16);
            host.trackInfo.hasColour = true;
            host.trackInfo.red = static_cast<uint8_t>(rgb >> 16);
            host.trackInfo.green = static_cast<uint8_t>(rgb >> 8);
            host.trackInfo.blue = static_cast<uint8_t>(rgb);
        }
        else if (arg == "--preset") {
            const int index = presets::findFactory(next);
            if (!host.presetLibrary() || index < 0 || !host.presetLibrary()->loadFactory(static_cast<size_t>(index))) {
                std::fprintf(stderr, "no factory preset \"%s\"\n", next);
                return 2;
            }
        } else if (arg == "--property") {
            const char* eq = std::strchr(next, '=');
            const int k = eq ? propertyByName(std::string(next, static_cast<size_t>(eq - next)).c_str()) : -1;
            if (k < 0 || !host.setProperty(properties::at(static_cast<size_t>(k)).id, eq + 1)) {
                std::fprintf(stderr, "cannot set property \"%s\"\n", next);
                return 2;
            }
        } else if (arg == "--set") {
            const char* eq = std::strchr(next, '=');
            const int p = eq ? paramByName(std::string(next, static_cast<size_t>(eq - next)).c_str()) : -1;
            double v = 0.0;
            if (p < 0 || params::at(static_cast<size_t>(p)).isOutput() ||
                !parseValue(params::at(static_cast<size_t>(p)), eq + 1, v)) {
                std::fprintf(stderr, "cannot set \"%s\"\n", next);
                return 2;
            }
            host.set(static_cast<size_t>(p), v);
        } else {
            return usage(), 2;
        }
    }

    Gui gui(host, product::info::kName, true);
    Capture capture;
    gui.window().onFrameDrawn = onFrame;
    gui.window().frameUser = &capture;
    if (!gui.create()) return std::fprintf(stderr, "the editor could not be created\n"), 1;
    gui.setUserScale(scale);
    if (!gui.setParent(0)) return std::fprintf(stderr, "the window could not be created\n"), 1;
    if (!gui.show()) return std::fprintf(stderr, "the window could not be shown\n"), 1;

    // X11 maps and exposes asynchronously: pace the wait rather than spin.
    auto pump = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            gui.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    };
    for (int i = 0; i < 300 && !capture.seen; ++i) pump(1);
    if (!capture.seen) {
        std::fprintf(stderr, "the editor never drew (is there a display?)\n");
        return 3;
    }
    const int frames = static_cast<int>(seconds * kTelemetryHz);
    for (int i = 0; i < frames; i += 4) {
        host.feed(4);
        pump(1);
    }

    EditorBase& editor = gui.editor();
    auto centre = [&](const char* name, float& x, float& y) {
        editor.idle(0.0);  // a preceding staged click may have changed an enabledWhen rule
        int c = -1;
        const Ui& ui = editor.layout();
        if (!std::strcmp(name, "Presets")) {
            for (int k = 0; k < ui.count() && c < 0; ++k)
                if (ui.control(k).widget == Widget::PresetBar) c = k;
        }
        const int k = propertyByName(name);
        for (int i = 0; k >= 0 && i < ui.count() && c < 0; ++i)
            if ((ui.control(i).widget == Widget::TextField || ui.control(i).widget == Widget::File) &&
                ui.control(i).param == properties::at(static_cast<size_t>(k)).id)
                c = i;
        const int p = paramByName(name);
        if (c < 0 && p >= 0) c = ui.find(params::at(static_cast<size_t>(p)).id);
        // A button, or anything else, by the label it was given.
        for (int i = 0; c < 0 && i < ui.count(); ++i)
            if (ui.control(i).label && !std::strcmp(ui.control(i).label, name)) c = i;
        if (c < 0) {
            std::fprintf(stderr, "no control for \"%s\"\n", name);
            return false;
        }
        if (!ui.shown(c) || !ui.interactive(c)) {
            std::fprintf(stderr, "control \"%s\" is hidden or disabled/non-interactive\n", name);
            return false;
        }
        const Rect r = ui.control(c).widget == Widget::PresetBar
            ? Ui::presetPart(ui.control(c).bounds, presetbar::kName) : ui.control(c).bounds;
        x = r.cx();
        y = r.cy();
        return true;
    };
    float x = 0, y = 0;
    // The first tab bar's page, as a click on that tab would choose it.
    for (int k = 0; page >= 0 && k < editor.layout().count(); ++k) {
        const Control& c = editor.layout().control(k);
        if (c.widget != Widget::Tabs) continue;
        const Rect r = Ui::tab(c.bounds, page, static_cast<int>(c.pages.size()));
        editor.pointerDown(r.cx(), r.cy(), 0, 0, false);
        editor.pointerUp(r.cx(), r.cy(), 0, 0);
        break;
    }
    if (focus) {
        if (!centre(focus, x, y)) return 2;
        editor.pointerDown(x, y, 0, 0, false);
        editor.pointerUp(x, y, 0, 0);
    }
    if (menu) {
        if (!centre(menu, x, y)) return 2;
        editor.pointerDown(x, y, 0, 0, false);
        if (!editor.layout().menu().open) {
            std::fprintf(stderr, "control \"%s\" has no menu\n", menu);
            return 2;
        }
    }
    if (context) {
        if (!centre(context, x, y)) return 2;
        editor.pointerDown(x, y, 1, 0, false);
        if (!editor.layout().menu().open) {
            std::fprintf(stderr, "control \"%s\" has no context menu\n", context);
            return 2;
        }
    }
    if (text) {
        if (!centre(text, x, y)) return 2;
        editor.pointerDown(x, y, 0, mod::kCtrl, false);
        if (propertyByName(text) >= 0) editor.key(key::kEnd, 0);  // the caret, not a selection
    }
    if (hover) {
        if (!centre(hover, x, y)) return 2;
        editor.pointerMove(x, y, 0);
    }
    // A key held down on the first keyboard, as the pointer would.
    for (int k = 0; play >= 0 && k < editor.layout().count(); ++k) {
        const Control& c = editor.layout().control(k);
        if (c.widget != Widget::Keyboard) continue;
        const Rect r = Ui::keyRect(c, play);
        editor.pointerDown(r.cx(), r.bottom() - 10.0f, 0, 0, false);
        break;
    }
    if (drop) editor.dropOver(editor.width() * 0.5f, editor.height() * 0.5f);
    // Held still, long enough for the tooltip.
    if (tooltip) {
        if (!centre(tooltip, x, y)) return 2;
        editor.pointerMove(x, y, 0);
        editor.idle(Ui::kTooltipSeconds + 0.1);
    }

    // Some window managers (KWin, for an XWayland window) put a newly mapped
    // top-level window back to the size it was created at, a little while
    // after mapping it. Insist until the size holds, and never write an
    // image at a scale nobody asked for.
    uint32_t wantW = 0, wantH = 0;
    gui.sizeAt(scale, wantW, wantH);
    auto sized = [&] { return gui.window().width() == wantW && gui.window().height() == wantH; };
    for (int held = 0, tries = 0; held < 10 && tries < 400; ++tries) {
        if (sized()) {
            ++held;
        } else {
            held = 0;
            gui.setUserScale(scale);
        }
        pump(1);
    }
    if (!sized()) {
        std::fprintf(stderr, "the window manager kept the preview at %ux%u, not %ux%u for --scale %g\n",
                     gui.window().width(), gui.window().height(), wantW, wantH, scale);
        return 5;
    }
    host.feed(2);
    editor.markDirty();
    capture.want = true;
    for (int i = 0; i < 300 && !capture.got; ++i) pump(1);
    if (!capture.got) return 4;
    // If a window manager sized the window anyway, Window letterboxed the
    // panel: crop to it, so the image is always the panel and only the panel.
    const float fit = std::min(static_cast<float>(capture.w) / editor.width(), static_cast<float>(capture.h) / editor.height());
    const int w = static_cast<int>(std::lround(editor.width() * fit));
    const int h = static_cast<int>(std::lround(editor.height() * fit));
    const int x0 = (capture.w - w) / 2, y0 = (capture.h - h) / 2;
    std::vector<unsigned char> panel(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int row = 0; row < h; ++row)
        std::memcpy(panel.data() + static_cast<size_t>(row) * static_cast<size_t>(w) * 3,
                    capture.rgb.data() + (static_cast<size_t>(y0 + row) * static_cast<size_t>(capture.w) + static_cast<size_t>(x0)) * 3,
                    static_cast<size_t>(w) * 3);
    if (!writePng(output, w, h, panel)) return 4;
    std::printf("wrote %s (%dx%d)\n", output, w, h);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage(), 2;
    const std::vector<std::string> rest(argv + std::min(argc, 3), argv + argc);
    if (!std::strcmp(argv[1], "--list")) {
        for (const Scenario& s : scenarios()) std::printf("%s\n", s.name.c_str());
        return 0;
    }
    if (!std::strcmp(argv[1], "--all")) {
        if (argc < 3) return usage(), 2;
#if !defined(_WIN32) && !defined(__APPLE__)
        if (!std::getenv("DISPLAY") || !*std::getenv("DISPLAY")) {
            std::printf("no display; nothing rendered\n");
            return 77;
        }
#endif
        std::filesystem::create_directories(argv[2]);
        int result = 0;
        for (const Scenario& s : scenarios()) {
            std::vector<std::string> args = s.args;
            args.insert(args.end(), rest.begin(), rest.end());
            const std::string out = (std::filesystem::path(argv[2]) / (s.name + ".png")).string();
            int r = preview(out.c_str(), args);
            // A virtual display can refuse one window now and then (seen
            // once under Xvfb on a CI runner): try that scenario again.
            if (r == 1) {
                std::fprintf(stderr, "scenario %s: trying again\n", s.name.c_str());
                r = preview(out.c_str(), args);
            }
            if (r != 0) {
                std::fprintf(stderr, "scenario %s failed (%d)\n", s.name.c_str(), r);
                result = r;
            }
        }
        return result;
    }
    if (argv[1][0] == '-') return usage(), 2;
    return preview(argv[1], std::vector<std::string>(argv + 2, argv + argc));
}
