#include "shell.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "supple/engine/realtime.h"
#include "supple/model/objects.h"
#include "supple/product.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/widgets.h"
#include "transport_bar.h"

namespace supple::gui {
namespace {

double nowMs() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

// "M Series Pro:capture_AUX0" -> "AUX0": the strip's button is ~58 px wide, so
// it shows the port's own name and the menu shows the full port.
std::string shortPort(const std::string& port) {
    const std::size_t colon = port.rfind(':');
    std::string tail = colon == std::string::npos ? port : port.substr(colon + 1);
    for (const std::string_view prefix : {"capture_", "playback_", "monitor_"})
        if (tail.rfind(prefix, 0) == 0) tail.erase(0, prefix.size());
    return tail.empty() ? port : tail;
}

// The view's channel string ("1", "1/2") back to numbers.
std::vector<int> channelList(std::string_view text) {
    std::vector<int> out;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t j = text.find('/', i);
        if (j == std::string_view::npos) j = text.size();
        try {
            out.push_back(std::stoi(std::string(text.substr(i, j - i))));
        } catch (...) {
            break;  // not a channel string: leave what we parsed
        }
        i = j + 1;
    }
    return out;
}

std::string portOf(const json::Value& inputs, int channel) {
    const json::Value* audio = inputs.find("audio");
    if (!audio || !audio->isArray()) return {};
    for (std::size_t i = 0; i < audio->size(); ++i) {
        const json::Value& e = audio->at(i);
        const json::Value* c = e.find("channel");
        const json::Value* p = e.find("port");
        if (c && p && static_cast<int>(c->asInt()) == channel) return p->asString();
    }
    return {};
}

}  // namespace

Shell::Shell(engine::EngineApp& app, cmd::Dispatcher& dispatcher, UiState& ui, std::string status,
             peaks::PeakService& peaks, MeterFeed& meters, InputsSource inputs)
    : app_(app),
      dispatcher_(dispatcher),
      ui_(ui),
      status_(std::move(status)),
      peaks_(peaks),
      meters_(meters),
      inputsSource_(std::move(inputs)) {}

Shell::~Shell() = default;

std::unique_ptr<ui::Node> Shell::build(ui::Root& root, AppActions& actions) {
    root_ = &root;
    auto shell = std::make_unique<ui::Node>("shell");
    shell->layout.dir = ui::Layout::Dir::Column;
    transport_ = std::make_unique<TransportBar>(app_, actions, dispatcher_, root);
    shell->add(transport_->build());
    auto& body = shell->emplace<ui::Node>("body");
    body.layout.dir = ui::Layout::Dir::Row;
    engine::EngineApp& app = app_;
    peaks::PeakService& peaks = peaks_;
    arrange_ = &body.emplace<views::ArrangeView>(
        peaks,
        [&app](const model::MediaSource& m) {
            const std::filesystem::path p(m.path);
            return p.is_absolute() ? p : app.mediaDir() / p;
        },
        [&app] {
            views::Transport t;
            t.loopEnabled = app.loop().enabled;
            t.loopStart = app.loop().start;
            t.loopEnd = app.loop().end;
            if (engine::Engine* e = app.engine()) {
                const engine::TransportState st = e->transport();
                t.playing = st.playing;
                t.seconds = st.seconds;
            }
            return t;
        });
    arrange_->layout.width = ui::SizeRule::fraction();
    // The TCP talks to the app through these: selection state, commands (with
    // a drag's gesture) and the meters it pools with its rows.
    views::TcpCallbacks tcp;
    tcp.selected = [this](const model::Id& track) { return isSelected(track); };
    tcp.select = [this](const model::Id& track, unsigned mods) { selectTrack(track, mods); };
    tcp.commit = [this](std::string_view command, json::Value args, std::string gesture) {
        commit(command, std::move(args), std::move(gesture));
    };
    tcp.addStrip = [this](std::string_view type) {
        json::Value args = json::Value::object();
        args["type"] = std::string(type);
        cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
        auto r = dispatcher_.execute({"track.add", std::move(args), std::move(origin)});
        if (!r) {
            if (onError) onError(r.error().message);
            return;
        }
        const std::string created = (*r)["track"].asString();
        ui_.selectedTracks = {created};  // §4.2: the new strip is selected
        selectAnchor_ = created;
    };
    tcp.meterShown = [this](const std::string& track, ui::Meter* meter) {
        meters_.attach(track, meter);
    };
    tcp.meterHidden = [this](ui::Meter* meter) { meters_.detach(meter); };
    // The input button: the app knows the device's port names, and it owns the
    // picker's popup and the engine.inputs list.
    tcp.inputText = [this](const model::Id& track, const std::string& channels) {
        return inputText(track, channels);
    };
    tcp.pickInput = [this](const model::Id& track, const ui::Rect& anchor) {
        openInputMenu(track, anchor);
    };
    arrange_->setTcpCallbacks(std::move(tcp));
    counts_ = &shell->emplace<ui::Label>("status", status_, ui::Font::Mono, "small", "ink-quiet");
    counts_->layout.width = ui::SizeRule::fraction();
    counts_->layout.height = ui::SizeRule::fixed(26);
    counts_->layout.padding = ui::Insets::xy(12, 0);
    shownVersion_ = ~std::uint64_t{0};
    return shell;
}

void Shell::installOverlay(ui::Root& root) {
    if (arrange_) arrange_->installOverlay(root);
}

void Shell::sync() {
    const model::Session& s = app_.head();
    const bool moved = s.version != shownVersion_;
    if (moved) {
        shownVersion_ = s.version;
        const int strips = static_cast<int>(s.tracks.size());
        counts_->setText(std::to_string(strips) + (strips == 1 ? " strip  " : " strips  ") +
                         "version " + std::to_string(s.version));
        arrange_->show(s);
    }
    // The bar changes with the model or while the engine runs; refreshing it on
    // every idle pass would repaint (and cost) for nothing.
    if (transport_ && (moved || app_.engine())) transport_->sync();
    // The input names track the engine (started or stopped), not the model.
    if (moved || app_.engine()) inputs();
}

const json::Value& Shell::inputs(bool force) {
    const double now = nowMs();
    if (!force && inputs_.isObject() && now - inputsAtMs_ < 500.0) return inputs_;
    inputsAtMs_ = now;
    if (inputsSource_) {
        inputs_ = inputsSource_();
    } else {
        cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
        auto r = dispatcher_.execute({"engine.inputs", json::Value::object(), std::move(origin)});
        inputs_ = r ? *r : json::Value::object();
    }
    if (!inputs_.isObject()) inputs_ = json::Value::object();
    return inputs_;
}

std::string Shell::inputText(const model::Id& track, const std::string& channels) const {
    const model::Session& s = app_.head();
    if (channels.empty()) {
        // Nothing on the audio channels: a MIDI input names its device.
        if (const model::Track* tr = s.tracks.find(track); tr && tr->record.input)
            if (const model::Route* r = s.routes.find(*tr->record.input); r && r->from.midiDevice)
                return *r->from.midiDevice == "*" ? std::string("midi *") : *r->from.midiDevice;
        return "in \u2014";
    }
    std::string out;
    for (const int ch : channelList(channels)) {
        std::string name = std::to_string(ch);
        if (const std::string port = portOf(inputs_, ch); !port.empty()) name = shortPort(port);
        out += out.empty() ? "" : "+";
        out += name;
    }
    return out.empty() ? ("in " + channels) : out;
}

void Shell::openInputMenu(const model::Id& track, const ui::Rect& anchor) {
    if (!root_) return;
    const json::Value& list = inputs(/*force=*/true);
    const model::Session& s = app_.head();
    const model::Track* tr = s.tracks.find(track);
    std::vector<int> current;
    std::string midiDevice;
    if (tr && tr->record.input)
        if (const model::Route* r = s.routes.find(*tr->record.input); r) {
            if (r->from.hwChannels) current = *r->from.hwChannels;
            if (r->from.midiDevice) midiDevice = *r->from.midiDevice;
        }
    auto pick = [this, track](json::Value args) {
        args["track"] = track.str();
        commit("track.set", std::move(args), {});
    };
    std::vector<ui::MenuItem> items;
    const json::Value* running = list.find("running");
    if (!running || !running->asBool()) {
        // The device only reports its ports while the engine holds it open
        // (engine.inputs), so say so rather than showing a stale list.
        items.push_back(
            {"engine stopped \u2014 start the engine to list its inputs", "", {}, false});
    } else {
        const json::Value* audio = list.find("audio");
        const std::size_t count = audio && audio->isArray() ? audio->size() : 0;
        for (std::size_t i = 0; i < count; ++i) {
            const json::Value& e = audio->at(i);
            const json::Value* c = e.find("channel");
            if (!c) continue;
            const int ch = static_cast<int>(c->asInt());
            const json::Value* p = e.find("port");
            const std::string port = p && p->isString() ? p->asString() : std::string();
            ui::MenuItem m;
            m.label = port.empty() ? ("input " + std::to_string(ch)) : port;
            m.shortcut = "in " + std::to_string(ch);
            m.checked = current.size() == 1 && current[0] == ch;
            m.run = [pick, ch] {
                json::Value args = json::Value::object();
                json::Value channels = json::Value::array();
                channels.push_back(json::Value(static_cast<std::int64_t>(ch)));
                args["record_input"] = std::move(channels);
                pick(std::move(args));
            };
            items.push_back(std::move(m));
        }
        // The adjacent pairs are what a stereo take uses ([n, n+1]).
        for (std::size_t i = 0; i + 1 < count; ++i) {
            const json::Value& a = audio->at(i);
            const json::Value& b = audio->at(i + 1);
            const json::Value* ca = a.find("channel");
            const json::Value* cb = b.find("channel");
            if (!ca || !cb) continue;
            const int first = static_cast<int>(ca->asInt()), second = static_cast<int>(cb->asInt());
            const json::Value* pa = a.find("port");
            const json::Value* pb = b.find("port");
            const std::string nameA = pa && pa->isString() ? pa->asString() : std::string();
            const std::string nameB = pb && pb->isString() ? pb->asString() : std::string();
            ui::MenuItem m;
            m.label = (nameA.empty() ? "input " + std::to_string(first) : nameA) + " + " +
                      (nameB.empty() ? "input " + std::to_string(second) : nameB);
            m.shortcut = "stereo " + std::to_string(first) + "-" + std::to_string(second);
            m.checked = current.size() == 2 && current[0] == first && current[1] == second;
            m.run = [pick, first, second] {
                json::Value args = json::Value::object();
                json::Value channels = json::Value::array();
                channels.push_back(json::Value(static_cast<std::int64_t>(first)));
                channels.push_back(json::Value(static_cast<std::int64_t>(second)));
                args["record_input"] = std::move(channels);
                pick(std::move(args));
            };
            items.push_back(std::move(m));
        }
        const json::Value* midi = list.find("midi");
        const std::size_t devices = midi && midi->isArray() ? midi->size() : 0;
        if (devices) {
            items.push_back({"", "", {}, true, true});  // separator
            ui::MenuItem any;
            any.label = "any MIDI device";
            any.shortcut = "midi";
            any.checked = midiDevice == "*";
            any.run = [pick] {
                json::Value args = json::Value::object();
                args["record_midi"] = "*";
                pick(std::move(args));
            };
            items.push_back(std::move(any));
            for (std::size_t i = 0; i < devices; ++i) {
                const json::Value& d = midi->at(i);
                const std::string name = d.isString() ? d.asString() : std::string();
                if (name.empty()) continue;
                ui::MenuItem m;
                m.label = name;
                m.shortcut = "midi";
                m.checked = midiDevice == name;
                m.run = [pick, name] {
                    json::Value args = json::Value::object();
                    args["record_midi"] = name;
                    pick(std::move(args));
                };
                items.push_back(std::move(m));
            }
        }
        if (items.empty())
            items.push_back({"the device reports no inputs", "", {}, false, false, false});
    }
    root_->openPopup(
        ui::makeMenu("tcp.input." + track.str(), std::move(items), [this] { root_->closePopup(); }),
        anchor);
}

bool Shell::isSelected(const model::Id& track) const {
    const std::string id = track.str();
    return std::find(ui_.selectedTracks.begin(), ui_.selectedTracks.end(), id) !=
           ui_.selectedTracks.end();
}

void Shell::selectTrack(const model::Id& track, unsigned mods) {
    const std::string id = track.str();
    auto& sel = ui_.selectedTracks;
    if (mods & ui::kShift) {  // extend from the anchor, in row order
        const auto& rows = arrange_->trackLayout().rows();
        auto index = [&](const std::string& want) {
            for (std::size_t i = 0; i < rows.size(); ++i)
                if (rows[i].track.str() == want) return static_cast<int>(i);
            return -1;
        };
        const std::string from = selectAnchor_.value_or(id);
        int a = index(from), b = index(id);
        if (a >= 0 && b >= 0) {
            if (a > b) std::swap(a, b);
            sel.clear();
            for (int i = a; i <= b; ++i)
                sel.push_back(rows[static_cast<std::size_t>(i)].track.str());
        } else {
            sel = {id};
        }
        selectAnchor_ = from;
    } else if (mods & ui::kCtrl) {  // toggle one
        const auto it = std::find(sel.begin(), sel.end(), id);
        if (it == sel.end())
            sel.push_back(id);
        else
            sel.erase(it);
        selectAnchor_ = id;
    } else {
        sel = {id};
        selectAnchor_ = id;
    }
    arrange_->tcpPanel().invalidate();  // the highlight is the panel's own
}

void Shell::commit(std::string_view command, json::Value args, std::string gesture) {
    cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}};
    origin.gesture = std::move(gesture);
    auto r = dispatcher_.execute({std::string(command), std::move(args), std::move(origin)});
    if (!r && onError) onError(r.error().message);
}

}  // namespace supple::gui
