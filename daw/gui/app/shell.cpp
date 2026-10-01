#include "shell.h"

#include <cctype>
#include <functional>

#include "supple/model/objects.h"
#include "supple/product.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

std::string_view typeToken(model::StripType t) {
    switch (t) {
        case model::StripType::Channel:
            return "channel";
        case model::StripType::Buss:
            return "buss";
        case model::StripType::Master:
            return "master";
    }
    return "ink";
}

// The strip tree: masters first, children indented under their parent; one
// node draws all rows (rows are cheap, and P5.4 virtualizes them).
class StripList : public ui::Node {
  public:
    explicit StripList(const engine::EngineApp& app) : Node("strips", ui::Role::List), app_(app) {}
    void paint(ui::Canvas& c) override {
        const model::Session& s = app_.head();
        const ui::Theme& t = c.theme();
        float y = bounds().y + t.space("s");
        constexpr float row = 30.0f;
        std::function<void(model::Id, int)> strip = [&](model::Id id, int depth) {
            const model::Track* tr = s.tracks.find(id);
            if (!tr || y > bounds().bottom()) return;
            const float x = bounds().x + t.space("l") + 18.0f * static_cast<float>(depth);
            const ui::Rect badge{x, y + 6, 22, row - 12};
            c.fillRound(badge, t.radius("s"), t.color(typeToken(tr->type)));
            const std::string letter(
                1, static_cast<char>(std::toupper(model::stripTypeName(tr->type).front())));
            c.text(badge, letter, ui::Font::UiBold, t.type("caption"), t.color("panel"),
                   ui::kCenter | ui::kMiddle);
            c.text({x + 32, y, bounds().right() - x - 40, row},
                   tr->name.empty() ? std::string(model::stripTypeName(tr->type)) : tr->name,
                   ui::Font::Ui, t.type("body"), t.color("ink"));
            y += row;
            for (model::Id child : tr->children) strip(child, depth + 1);
        };
        c.save();
        c.clip(bounds());
        for (model::Id master : s.project->masters) strip(master, 0);
        c.restore();
    }

  private:
    const engine::EngineApp& app_;
};

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

Shell::Shell(const engine::EngineApp& app, std::string status, peaks::PeakService& peaks)
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
    strips_ = &body.emplace<StripList>(app_);
    strips_->layout.width = ui::SizeRule::fixed(220);
    const engine::EngineApp& app = app_;
    lanes_ = &body.emplace<views::Lanes>(peaks_, [&app](const model::MediaSource& m) {
        const std::filesystem::path p(m.path);
        return p.is_absolute() ? p : app.mediaDir() / p;
    });
    auto& foot = root->emplace<ui::Label>("status", status_, ui::Font::Mono, "small", "ink-quiet");
    foot.layout.width = ui::SizeRule::fraction();
    foot.layout.height = ui::SizeRule::fixed(28);
    foot.layout.padding = ui::Insets::xy(16, 0);
    shownVersion_ = ~std::uint64_t{0};
    return root;
}

void Shell::sync() {
    const model::Session& s = app_.head();
    if (s.version == shownVersion_) return;
    shownVersion_ = s.version;
    counts_->setText(std::to_string(s.tracks.size()) + " strips   version " +
                     std::to_string(s.version));
    strips_->invalidate();
    lanes_->show(s);
}

}  // namespace supple::gui
