#include "routing_matrix.h"

#include <nanovg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "supple/ui/popup.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};
constexpr float kLabelW = 190, kHeaderH = 112, kCell = 22;

std::string fmt(double v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

}  // namespace

std::string MatrixModel::blocked(std::size_t row, std::size_t col) const {
    const Source& s = sources[row];
    const Destination& d = destinations[col];
    if (d.kind == "track") {
        if (s.id == d.id) return "a strip cannot send to itself";
        if (!s.rule.empty()) return s.rule;
        if (!d.rule.empty()) return d.rule;
    } else if (s.id == d.track) {
        return "a strip cannot key a plug-in in its own chain";
    }
    for (std::size_t i = 0; i < cyclePairs.size(); ++i)
        if (cyclePairs[i].first == s.id && cyclePairs[i].second == d.id)
            return "it would feed back: " + cyclePaths[i];
    return {};
}

std::vector<const MatrixModel::Send*> MatrixModel::sendsAt(std::size_t row, std::size_t col) const {
    std::vector<const Send*> out;
    for (const Send& x : sends)
        if (x.from == sources[row].id && x.to == destinations[col].id) out.push_back(&x);
    return out;
}

namespace {

class Grid : public ui::Node {
  public:
    Grid(const MatrixModel& m, std::function<void(std::size_t, std::size_t)> click,
         std::function<void(std::optional<std::pair<std::size_t, std::size_t>>)> hover,
         const std::optional<std::string>& selected)
        : Node("routing.grid"),
          m_(m),
          click_(std::move(click)),
          hover_(std::move(hover)),
          selected_(selected) {
        interactive = true;
        layout.height = ui::SizeRule::fraction();
    }

    std::optional<std::pair<std::size_t, std::size_t>> cellAt(ui::Point p) const {
        const ui::Rect b = bounds();
        const float x = p.x - (b.x + kLabelW) + scrollX_, y = p.y - (b.y + kHeaderH) + scrollY_;
        if (p.x < b.x + kLabelW || p.y < b.y + kHeaderH || x < 0 || y < 0) return std::nullopt;
        const auto col = static_cast<std::size_t>(x / kCell),
                   row = static_cast<std::size_t>(y / kCell);
        if (row >= m_.sources.size() || col >= m_.destinations.size()) return std::nullopt;
        return std::pair{row, col};
    }

    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        const ui::Rect b = bounds();
        c.fillRect(b, t.color("panel"));
        const float gx = b.x + kLabelW - scrollX_, gy = b.y + kHeaderH - scrollY_;
        const ui::Rect cells{b.x + kLabelW, b.y + kHeaderH, b.w - kLabelW, b.h - kHeaderH};
        auto typeColor = [&](const std::string& type) {
            return t.color(type == "master" ? "master" : type == "buss" ? "buss" : "channel");
        };
        // Column headers: rotated names.
        c.save();
        c.clip({cells.x, b.y, cells.w, kHeaderH});
        NVGcontext* vg = c.vg();
        for (std::size_t col = 0; col < m_.destinations.size(); ++col) {
            const auto& d = m_.destinations[col];
            const float x = gx + static_cast<float>(col) * kCell;
            if (x + kCell < cells.x || x > cells.right()) continue;
            c.fillRect({x + 4, b.y + kHeaderH - 6, kCell - 8, 3},
                       d.kind == "key" ? t.color("agent") : typeColor(d.type));
            nvgSave(vg);
            nvgTranslate(vg, x + kCell / 2 + 4, b.y + kHeaderH - 10);
            nvgRotate(vg, -static_cast<float>(M_PI) / 3);
            c.text({0, -8, kHeaderH * 1.05f, 16}, d.name.empty() ? d.id : d.name, ui::Font::Ui,
                   t.type("caption"), d.kind == "key" ? t.color("ink-quiet") : t.color("ink"),
                   ui::kLeft | ui::kMiddle);
            nvgRestore(vg);
        }
        c.restore();
        // Row labels: indented by depth, type colour bar.
        c.save();
        c.clip({b.x, cells.y, kLabelW, cells.h});
        for (std::size_t row = 0; row < m_.sources.size(); ++row) {
            const auto& s = m_.sources[row];
            const float y = gy + static_cast<float>(row) * kCell;
            if (y + kCell < cells.y || y > cells.bottom()) continue;
            const float indent = 8 + static_cast<float>(s.depth) * 12;
            c.fillRect({b.x + indent - 6, y + 4, 3, kCell - 8}, typeColor(s.type));
            c.text({b.x + indent, y, kLabelW - indent - 6, kCell}, s.name.empty() ? s.id : s.name,
                   ui::Font::Ui, t.type("small"), t.color("ink"));
        }
        c.restore();
        // Cells.
        c.save();
        c.clip(cells);
        for (std::size_t row = 0; row < m_.sources.size(); ++row) {
            const float y = gy + static_cast<float>(row) * kCell;
            if (y + kCell < cells.y || y > cells.bottom()) continue;
            for (std::size_t col = 0; col < m_.destinations.size(); ++col) {
                const float x = gx + static_cast<float>(col) * kCell;
                if (x + kCell < cells.x || x > cells.right()) continue;
                const ui::Rect r{x + 1, y + 1, kCell - 2, kCell - 2};
                const auto sends = m_.sendsAt(row, col);
                const bool blocked = sends.empty() && !m_.blocked(row, col).empty();
                const bool hovered = hovered_ && hovered_->first == row && hovered_->second == col;
                if (blocked) {
                    c.fillRect(r, t.color("ground").withAlpha(0.7f));
                    c.line({r.x + 4, r.bottom() - 4}, {r.right() - 4, r.y + 4}, t.color("rule"), 1);
                } else {
                    c.fillRect(r, hovered ? t.color("selection") : t.color("raised"));
                }
                if (!sends.empty()) {
                    const bool sel = selected_ && *selected_ == sends.front()->id;
                    const ui::Rect dot{r.x + 4, r.y + 4, r.w - 8, r.h - 8};
                    c.fillRound(dot, 3,
                                sends.front()->mute ? t.color("ink-faint") : t.color("accent"));
                    if (sel)
                        c.strokeRound({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, 3, t.color("focus"), 2);
                }
                // The implicit parent send, outlined.
                if (m_.destinations[col].kind == "track" &&
                    m_.sources[row].parent == m_.destinations[col].id && m_.sources[row].parentSend)
                    c.strokeRect({r.x + 2, r.y + 2, r.w - 4, r.h - 4}, t.color("ink-quiet"));
            }
        }
        c.restore();
        c.fillRect({cells.x - 1, b.y, 1, b.h}, t.color("rule"));
        c.fillRect({b.x, cells.y - 1, b.w, 1}, t.color("rule"));
    }

    bool onPointer(const ui::PointerEvent& e) override {
        using T = ui::PointerEvent::Type;
        if (e.type == T::Move || e.type == T::Enter) {
            const auto cell = cellAt(e.pos);
            if (cell != hovered_) {
                hovered_ = cell;
                hover_(cell);
                invalidate();
            }
            return true;
        }
        if (e.type == T::Leave) {
            hovered_.reset();
            hover_(std::nullopt);
            invalidate();
            return true;
        }
        if (e.type == T::Down && e.button == 1) {
            if (auto cell = cellAt(e.pos)) click_(cell->first, cell->second);
            return true;
        }
        if (e.type == T::Wheel) {
            const float maxX = std::max(
                0.0f, static_cast<float>(m_.destinations.size()) * kCell - (bounds().w - kLabelW));
            const float maxY = std::max(
                0.0f, static_cast<float>(m_.sources.size()) * kCell - (bounds().h - kHeaderH));
            if (e.mods & ui::kShift)
                scrollX_ = std::clamp(scrollX_ - (e.wheelY + e.wheelX) * kCell, 0.0f, maxX);
            else {
                scrollY_ = std::clamp(scrollY_ - e.wheelY * kCell * 2, 0.0f, maxY);
                scrollX_ = std::clamp(scrollX_ - e.wheelX * kCell, 0.0f, maxX);
            }
            invalidate();
            return true;
        }
        return false;
    }
    // A cell's centre in window coordinates (tests).
    ui::Point centre(std::size_t row, std::size_t col) const {
        const ui::Rect b = bounds();
        return {b.x + kLabelW - scrollX_ + (static_cast<float>(col) + 0.5f) * kCell,
                b.y + kHeaderH - scrollY_ + (static_cast<float>(row) + 0.5f) * kCell};
    }

  private:
    const MatrixModel& m_;
    std::function<void(std::size_t, std::size_t)> click_;
    std::function<void(std::optional<std::pair<std::size_t, std::size_t>>)> hover_;
    const std::optional<std::string>& selected_;
    std::optional<std::pair<std::size_t, std::size_t>> hovered_;
    float scrollX_ = 0, scrollY_ = 0;
};

}  // namespace

RoutingMatrix::RoutingMatrix(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
                             engine::EngineApp& app)
    : root_(root), actions_(actions), dispatcher_(dispatcher), app_(app) {
    actions_.addUi("ui.routing", "View: routing matrix", [this] { open(); });
}

Result<json::Value> RoutingMatrix::exec(const std::string& command, json::Value args) {
    auto r = dispatcher_.execute({command, std::move(args), kOrigin});
    if (!r && status_) status_->setText(command + ": " + r.error().message);
    return r;
}

Status RoutingMatrix::reload() {
    auto m = dispatcher_.execute({"route.matrix", json::Value::object(), kOrigin});
    if (!m) return std::move(m).error();
    auto list = dispatcher_.execute({"route.list", json::Value::object(), kOrigin});
    if (!list) return std::move(list).error();
    MatrixModel next;
    auto str = [](const json::Value& v, const char* k) {
        const json::Value* x = v.find(k);
        return x && x->isString() ? x->asString() : std::string();
    };
    for (const auto& s : m->find("sources")->asArray()) {
        MatrixModel::Source src;
        src.id = str(s, "id");
        src.name = str(s, "name");
        src.type = str(s, "type");
        src.parent = str(s, "parent");
        src.depth = static_cast<int>(s.find("depth")->asInt());
        if (const auto* ps = s.find("parent_send")) src.parentSend = ps->asBool();
        if (const auto* rule = s.find("rule")) src.rule = str(*rule, "message");
        next.sources.push_back(std::move(src));
    }
    for (const auto& d : m->find("destinations")->asArray()) {
        MatrixModel::Destination dst;
        dst.id = str(d, "id");
        dst.name = str(d, "name");
        dst.kind = str(d, "kind");
        dst.type = str(d, "type");
        dst.track = str(d, "track");
        if (const auto* rule = d.find("rule")) dst.rule = str(*rule, "message");
        next.destinations.push_back(std::move(dst));
    }
    for (const auto& c : m->find("cycles")->asArray()) {
        next.cyclePairs.emplace_back(str(c, "from"), str(c, "to"));
        next.cyclePaths.push_back(str(c, "path"));
    }
    for (const auto& r : list->find("routes")->asArray()) {
        MatrixModel::Send x;
        x.id = str(r, "id");
        x.from = str(r, "from");
        x.to = str(r, "fx").empty() ? str(r, "to") : str(r, "fx");
        x.tap = str(r, "tap");
        if (const auto* v = r.find("volume_db"); v && v->isNumber()) x.volumeDb = v->asDouble();
        if (const auto* v = r.find("pan"); v && v->isNumber()) x.pan = v->asDouble();
        if (const auto* v = r.find("mute"); v && v->isBool()) x.mute = v->asBool();
        next.sends.push_back(std::move(x));
    }
    model_ = std::move(next);
    version_ = app_.head().version;
    if (selected_ && std::none_of(model_.sends.begin(), model_.sends.end(),
                                  [&](const MatrixModel::Send& x) { return x.id == *selected_; }))
        selected_.reset();
    select(selected_);
    if (grid_) grid_->invalidate();
    return {};
}

void RoutingMatrix::open() {
    if (isOpen()) return;
    auto panel = std::make_unique<ui::Node>("routing");
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.width = ui::SizeRule::fixed(860);
    panel->layout.height = ui::SizeRule::fixed(560);
    panel->layout.padding = ui::Insets::all(10);
    panel->layout.gap = 8;
    auto& header = panel->emplace<ui::Node>("routing.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(24);
    header.layout.align = ui::Layout::Align::Center;
    auto& title = header.emplace<ui::Label>("routing.title", "Routing", ui::Font::UiBold, "body");
    title.layout.width = ui::SizeRule::fraction();
    header.emplace<ui::Button>("routing.close", "Close", [this] { root_.closePopup(); });
    auto& grid = panel->emplace<Grid>(
        model_, [this](std::size_t row, std::size_t col) { (void)clickCell(row, col); },
        [this](std::optional<std::pair<std::size_t, std::size_t>> cell) {
            if (!status_) return;
            if (!cell) {
                status_->setText(status());
                return;
            }
            const auto& s = model_.sources[cell->first];
            const auto& d = model_.destinations[cell->second];
            const std::string pair =
                (s.name.empty() ? s.id : s.name) + " → " + (d.name.empty() ? d.id : d.name);
            const auto sends = model_.sendsAt(cell->first, cell->second);
            if (!sends.empty())
                status_->setText(pair + ": " + fmt(sends.front()->volumeDb, 1) + " dB, " +
                                 sends.front()->tap + (sends.front()->mute ? ", muted" : "") +
                                 " — click to edit");
            else if (const std::string why = model_.blocked(cell->first, cell->second);
                     !why.empty())
                status_->setText(pair + ": not allowed — " + why);
            else if (d.kind == "track" && s.parent == d.id && s.parentSend)
                status_->setText(
                    pair +
                    ": the parent send (outlined; track.move changes the parent) — click to "
                    "add a separate send");
            else
                status_->setText(pair + ": click to send");
        },
        selected_);
    grid_ = &grid;
    details_ = &panel->emplace<ui::Node>("routing.details");
    details_->layout.dir = ui::Layout::Dir::Row;
    details_->layout.height = ui::SizeRule::fixed(28);
    details_->layout.gap = 6;
    details_->layout.align = ui::Layout::Align::Center;
    sendTitle_ = &details_->emplace<ui::Label>("routing.send", "", ui::Font::UiBold, "small");
    sendTitle_->layout.width = ui::SizeRule::fraction();
    for (auto [id, label, tap] : {std::tuple{"routing.tap.post", "Post", "post_fader"},
                                  {"routing.tap.pre", "Pre", "pre_fader"},
                                  {"routing.tap.prefx", "Pre-FX", "pre_fx"}}) {
        const std::string value = tap;
        taps_.push_back(&details_->emplace<ui::Button>(
            id, label, [this, value] { setSend("tap", json::Value(value)); }));
    }
    volume_ = &details_->emplace<ui::TextField>("routing.volume", "", "dB");
    volume_->layout.width = ui::SizeRule::fixed(80);
    volume_->onSubmit = [this](const std::string& text) {
        setSend("volume_db", json::Value(text));
    };
    pan_ = &details_->emplace<ui::TextField>("routing.pan", "", "pan %");
    pan_->layout.width = ui::SizeRule::fixed(80);
    pan_->onSubmit = [this](const std::string& text) { setSend("pan", json::Value(text)); };
    mute_ = &details_->emplace<ui::Button>("routing.mute", "Mute", [this] {
        for (const auto& x : model_.sends)
            if (selected_ && x.id == *selected_) setSend("mute", json::Value(!x.mute));
    });
    details_->emplace<ui::Button>("routing.remove", "Remove", [this] {
        if (!selected_) return;
        json::Value args = json::Value::object();
        args["route"] = *selected_;
        if (exec("route.remove", std::move(args))) {
            selected_.reset();
            (void)reload();
        }
    });
    status_ = &panel->emplace<ui::Label>("routing.status", "", ui::Font::Ui, "small", "ink-quiet");
    status_->layout.height = ui::SizeRule::fixed(18);
    root_.openPopup(std::move(panel), {}, ui::Placement::Center, [this] {
        grid_ = nullptr;
        status_ = sendTitle_ = nullptr;
        details_ = nullptr;
        volume_ = pan_ = nullptr;
        mute_ = nullptr;
        taps_.clear();
        selected_.reset();
    });
    if (auto r = reload(); !r && status_) status_->setText(r.error().message);
    if (status_) status_->setText(status());
}

std::string RoutingMatrix::status() const {
    return std::to_string(model_.sends.size()) +
           " sends · hover a cell for its rule, click to send or edit";
}

void RoutingMatrix::select(std::optional<std::string> send) {
    selected_ = std::move(send);
    if (!details_) return;
    const MatrixModel::Send* x = nullptr;
    for (const auto& s : model_.sends)
        if (selected_ && s.id == *selected_) x = &s;
    for (const auto& n : details_->children()) n->enabled = x != nullptr || n.get() == sendTitle_;
    if (!x) {
        sendTitle_->setText("Select a send to edit it");
        volume_->setText("");
        pan_->setText("");
        return;
    }
    auto nameOf = [&](const std::string& id) {
        for (const auto& s : model_.sources)
            if (s.id == id) return s.name.empty() ? id : s.name;
        for (const auto& d : model_.destinations)
            if (d.id == id) return d.name.empty() ? id : d.name;
        return id;
    };
    sendTitle_->setText(nameOf(x->from) + " → " + nameOf(x->to) + (x->mute ? " (muted)" : ""));
    const char* names[] = {"post_fader", "pre_fader", "pre_fx"};
    const char* labels[] = {"Post", "Pre", "Pre-FX"};
    for (std::size_t i = 0; i < taps_.size(); ++i)
        taps_[i]->setText(std::string(x->tap == names[i] ? "• " : "") + labels[i]);
    if (!volume_->focused()) volume_->setText(fmt(x->volumeDb, 1) + " dB");
    if (!pan_->focused()) pan_->setText(fmt(x->pan, 0) + "%");
    mute_->setText(x->mute ? "Unmute" : "Mute");
    if (grid_) grid_->invalidate();
}

void RoutingMatrix::setSend(const char* field, json::Value value) {
    if (!selected_) return;
    json::Value args = json::Value::object();
    args["route"] = *selected_;
    args[field] = std::move(value);
    if (exec("route.set", std::move(args))) (void)reload();
}

Status RoutingMatrix::clickCell(std::size_t row, std::size_t col) {
    if (row >= model_.sources.size() || col >= model_.destinations.size())
        return Error("gui.routing", "no such cell");
    const auto sends = model_.sendsAt(row, col);
    if (!sends.empty()) {
        select(sends.front()->id);
        return {};
    }
    if (const std::string why = model_.blocked(row, col); !why.empty()) {
        if (status_) status_->setText("Not allowed: " + why);
        return Error("route.strip_type", why);
    }
    json::Value args = json::Value::object();
    args["from"] = model_.sources[row].id;
    const auto& d = model_.destinations[col];
    args[d.kind == "key" ? "fx" : "to"] = d.id;
    auto r = exec("route.add", std::move(args));
    if (!r) return std::move(r).error();
    selected_ = (*r)["id"].asString();
    return reload();
}

void RoutingMatrix::sync() {
    if (isOpen() && app_.head().version != version_) (void)reload();
}

}  // namespace supple::gui
