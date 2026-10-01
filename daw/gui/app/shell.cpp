#include "shell.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <utility>

#include "supple/engine/realtime.h"
#include "supple/model/objects.h"
#include "supple/product.h"
#include "supple/ui/root.h"
#include "supple/ui/widgets.h"
#include "transport_bar.h"

namespace supple::gui {

Shell::Shell(engine::EngineApp& app, cmd::Dispatcher& dispatcher, UiState& ui, std::string status,
             peaks::PeakService& peaks, MeterFeed& meters)
    : app_(app),
      dispatcher_(dispatcher),
      ui_(ui),
      status_(std::move(status)),
      peaks_(peaks),
      meters_(meters) {}

Shell::~Shell() = default;

std::unique_ptr<ui::Node> Shell::build(ui::Root& root, AppActions& actions) {
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
    if (s.version != shownVersion_) {
        shownVersion_ = s.version;
        const int strips = static_cast<int>(s.tracks.size());
        counts_->setText(std::to_string(strips) + (strips == 1 ? " strip  " : " strips  ") +
                         "version " + std::to_string(s.version));
        arrange_->show(s);
    }
    if (transport_) transport_->sync();
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
