#include "shell.h"

#include <filesystem>
#include <functional>

#include "supple/engine/realtime.h"
#include "supple/model/objects.h"
#include "supple/product.h"
#include "supple/ui/root.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

// The transport bar's panel and the rule under it.
class Bar : public ui::Node {
  public:
    using Node::Node;
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().bottom() - 1, bounds().w, 1}, c.theme().color("rule"));
    }
};

}  // namespace

Shell::Shell(engine::EngineApp& app, std::string status, peaks::PeakService& peaks)
    : app_(app), status_(std::move(status)), peaks_(peaks) {}

std::unique_ptr<ui::Node> Shell::build() {
    auto root = std::make_unique<ui::Node>("shell");
    root->layout.dir = ui::Layout::Dir::Column;
    auto& bar = root->emplace<Bar>("transport");
    bar.layout.dir = ui::Layout::Dir::Row;
    bar.layout.height = ui::SizeRule::fixed(44);
    bar.layout.padding = ui::Insets::xy(16, 0);
    bar.layout.gap = 24;
    bar.emplace<ui::Label>("product", std::string(product::kName), ui::Font::UiBold, "title");
    bar.emplace<ui::Label>("position", "1.1.000", ui::Font::MonoBold, "title");
    bar.emplace<ui::Node>("spacer");  // takes the remaining width
    counts_ = &bar.emplace<ui::Label>("counts", "", ui::Font::Mono, "mono", "ink-quiet");
    auto& body = root->emplace<ui::Node>("body");
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
    auto& foot = root->emplace<ui::Label>("status", status_, ui::Font::Mono, "small", "ink-quiet");
    foot.layout.width = ui::SizeRule::fraction();
    foot.layout.height = ui::SizeRule::fixed(28);
    foot.layout.padding = ui::Insets::xy(16, 0);
    shownVersion_ = ~std::uint64_t{0};
    return root;
}

void Shell::installOverlay(ui::Root& root) {
    if (arrange_) arrange_->installOverlay(root);
}

void Shell::sync() {
    const model::Session& s = app_.head();
    if (s.version == shownVersion_) return;
    shownVersion_ = s.version;
    counts_->setText(std::to_string(s.tracks.size()) + " strips   version " +
                     std::to_string(s.version));
    arrange_->show(s);
}

}  // namespace supple::gui
