// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "supple/suite/route_list.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "gui/EditorHost.h"
#include "gui/Widgets.h"
#include "supple/script_code.h"

namespace supple::suite {
namespace {

constexpr float kHeader = 22.0f;  // the columns' captions
constexpr float kRow = 26.0f, kRowGap = 3.0f;
constexpr float kArrow = 22.0f, kColumnGap = 8.0f, kRemove = 18.0f;

NVGcolor faded(uint32_t colour, float alpha) {
    NVGcolor c = gui::rgb(colour);
    c.a = alpha;
    return c;
}

// A small chevron pointing down: a menu's.
void chevron(NVGcontext* vg, float x, float y, NVGcolor colour) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, x - 3.0f, y - 1.5f);
    nvgLineTo(vg, x, y + 1.5f);
    nvgLineTo(vg, x + 3.0f, y - 1.5f);
    nvgStrokeColor(vg, colour);
    nvgStrokeWidth(vg, 1.2f);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);
}

// Text in `face` at `size` from (x, y), cut to `width` with an ellipsis.
void fitted(NVGcontext* vg, float x, float y, std::string text, float size, float width,
            const char* face, NVGcolor colour) {
    const float at = gui::widgets::fitText(vg, text, size, size - 1.5f, width, face);
    nvgSave(vg);
    nvgFontFace(vg, face);
    nvgFontSize(vg, at);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, colour);
    nvgText(vg, x, y, text.c_str(), nullptr);
    nvgRestore(vg);
}

std::string key(const RouteList::Row& r) { return r.source + ">" + r.destination; }

}  // namespace

RouteList::RouteList(const Definition& definition, float width, float height)
    : definition_(definition), codec_(definition), width_(width), height_(height) {
    const Meaning& m = codec_.meaning();
    if (m.routes >= 0 && static_cast<size_t>(m.routes) < definition_.properties().count())
        property_ = definition_.properties().at(static_cast<size_t>(m.routes)).id;
}

bool RouteList::shows(const Definition& definition) {
    const Meaning m = meaningOf(definition);
    return m.routes >= 0 && static_cast<size_t>(m.routes) < definition.properties().count();
}

void RouteList::open(gui::EditorHost& host, gui::Ui& ui, gui::Rect body) {
    host_ = &host;
    ui_ = &ui;
    body_ = body;
    idle();
}

bool RouteList::idle() {
    if (!host_ || host_->propertiesVersion() == version_) return false;
    version_ = host_->propertiesVersion();
    const std::string_view text = host_->property(property_);
    if (text == text_) return false;
    read();
    return true;
}

// --- where things are -----------------------------------------------------------------------

int RouteList::visibleRows() const {
    return std::max(1, static_cast<int>((body_.h - kHeader + kRowGap) / (kRow + kRowGap)));
}

gui::Rect RouteList::rowRect(int row) const {
    return {body_.x, body_.y + kHeader + static_cast<float>(row - scroll_) * (kRow + kRowGap),
            body_.w, kRow};
}

gui::Rect RouteList::partRect(int row, Part part) const {
    const gui::Rect r = rowRect(row);
    const float w = r.w - kRemove - 3.0f * kColumnGap - kArrow;
    const float source = std::round(w * 0.24f), destination = std::round(w * 0.34f),
                amount = std::round(w * 0.24f), via = w - source - destination - amount;
    float x = r.x;
    auto take = [&](float width, float gap) {
        const gui::Rect cell{x, r.y, width, r.h};
        x += width + gap;
        return cell;
    };
    const gui::Rect cells[] = {take(source, kArrow), take(destination, kColumnGap),
                               take(amount, kColumnGap), take(via, kColumnGap),
                               take(kRemove, 0.0f)};
    switch (part) {
        case Part::Source:
            return cells[0];
        case Part::Destination:
            return cells[1];
        case Part::Amount:
            return cells[2].inset(6.0f, 5.0f);
        case Part::Via:
            return cells[3];
        case Part::Remove:
            return cells[4];
        default:
            return r;
    }
}

// The row after the routes.
gui::Rect RouteList::addRect() const { return rowRect(static_cast<int>(rows_.size())); }

RouteList::Hit RouteList::hit(float x, float y) const {
    if (y < body_.y + kHeader || y >= body_.bottom()) return {};
    if (addRect().contains(x, y)) return {Part::Add, -1};
    const int last = std::min(static_cast<int>(rows_.size()), scroll_ + visibleRows());
    for (int row = scroll_; row < last; ++row) {
        if (!rowRect(row).contains(x, y)) continue;
        for (Part p : {Part::Source, Part::Destination, Part::Via, Part::Remove})
            if (partRect(row, p).contains(x, y)) return {p, row};
        // The bar takes its whole cell.
        if (partRect(row, Part::Amount).inset(-6.0f, -5.0f).contains(x, y))
            return {Part::Amount, row};
        return {Part::None, row};
    }
    return {};
}

// --- the routes -----------------------------------------------------------------------------

void RouteList::read() {
    text_ = std::string(host_->property(property_));
    std::vector<Row> now;
    forEachRoute(text_, [&](const RouteLine& r) {
        now.push_back(
            {std::string(r.source), std::string(r.destination), std::string(r.via), r.amount});
    });
    // The rows still there, in their order, then the new ones.
    std::vector<Row> rows;
    for (const Row& old : rows_)
        for (const Row& r : now)
            if (key(r) == key(old)) rows.push_back(r);
    for (const Row& r : now)
        if (std::none_of(rows.begin(), rows.end(), [&](const Row& k) { return key(k) == key(r); }))
            rows.push_back(r);
    rows_ = std::move(rows);
    scroll_ =
        std::clamp(scroll_, 0, std::max(0, static_cast<int>(rows_.size()) + 1 - visibleRows()));
    if (dragRow_ >= static_cast<int>(rows_.size())) dragRow_ = -1;
}

bool RouteList::write(std::vector<Row> rows) {
    if (!host_) return false;
    json::Value routes = json::Value::object();
    for (const Row& r : rows) {
        json::Value route = json::Value::object();
        route["amount"] = r.amount;
        if (!r.via.empty()) route["via"] = r.via;
        routes[key(r)] = std::move(route);
    }
    std::string text;
    std::vector<json::Error> errors;
    if (!codec_.routeText(routes, "/routes", text, errors)) return false;
    if (text != text_ && !host_->setProperty(property_, text)) return false;
    rows_ = std::move(rows);
    text_ = std::move(text);
    version_ = host_->propertiesVersion();
    return true;
}

int RouteList::paramIndex(const std::string& k) const {
    const Meaning& m = codec_.meaning();
    for (size_t i = 0; i < m.params.size(); ++i)
        if (m.params[i].key == k) return static_cast<int>(i);
    return -1;
}

const SourceMeaning* RouteList::source(const std::string& k) const {
    for (const SourceMeaning& s : codec_.meaning().sources)
        if (s.key == k) return &s;
    return nullptr;
}

bool RouteList::builtIn(const std::string& k) const {
    return std::any_of(std::begin(script::detail::kInstrumentSources),
                       std::end(script::detail::kInstrumentSources),
                       [&](const script::detail::BuiltinSource& b) { return k == b.key; });
}

bool RouteList::allowed(const std::string& from, const std::string& via, size_t index,
                        int except) const {
    const Meaning& m = codec_.meaning();
    if (index >= m.params.size() || m.params[index].routable == Routable::No) return false;
    const SourceMeaning* s = source(from);
    const SourceMeaning* v = via.empty() ? nullptr : source(via);
    if (!s || (!via.empty() && !v)) return false;
    if ((s->perVoice || (v && v->perVoice)) && m.params[index].routable != Routable::Voice)
        return false;
    for (size_t r = 0; r < rows_.size(); ++r)
        if (static_cast<int>(r) != except && rows_[r].source == from &&
            rows_[r].destination == m.params[index].key)
            return false;
    return true;
}

double RouteList::reach(size_t index) const {
    return routeReach(definition_.params().at(index), codec_.meaning().params[index]);
}

const char* RouteList::unit(size_t index) const {
    return routeAmountUnit(definition_.params().at(index));
}

int RouteList::decimals(size_t index) const {
    const double r = reach(index);
    const char* u = unit(index);
    if (u && std::strcmp(u, "oct") == 0) return 2;
    if (!(r > 0.0)) return 2;
    return std::clamp(2 - static_cast<int>(std::floor(std::log10(r))), 0, 2);
}

double RouteList::rounded(size_t index, double amount) const {
    const double step = std::pow(10.0, -decimals(index));
    const double r = reach(index);
    return std::clamp(std::round(amount / step) * step, -r, r);
}

std::string RouteList::amountText(size_t index, double amount) const {
    char text[48];
    const double shown = std::fabs(amount) < 1e-12 ? 0.0 : amount;
    const char* u = unit(index);
    std::snprintf(text, sizeof(text), "%+.*f%s%s", decimals(index), shown, u ? " " : "",
                  u ? u : "");
    return text;
}

std::string RouteList::shortName(size_t index) const {
    // "Filter 1 Cutoff" in the group "Filter 1" is its cutoff.
    const ParamDef& p = definition_.params().at(index);
    const std::string_view name = p.name, group = p.group ? p.group : "";
    if (!group.empty() && name.size() > group.size() + 1 &&
        name.compare(0, group.size(), group) == 0 && name[group.size()] == ' ')
        return std::string(name.substr(group.size() + 1));
    return std::string(name);
}

std::string RouteList::sourceName(const std::string& k) const {
    const SourceMeaning* s = source(k);
    return s ? s->name : k;
}

// --- menus ----------------------------------------------------------------------------------

void RouteList::chooseSource(int row, gui::Rect anchor) {
    std::vector<gui::MenuItem> items;
    std::vector<std::string> keys;  // each item's source, "" for a header
    const Meaning& m = codec_.meaning();
    // Whether `from` could start a route: to this row's parameter, or a new
    // route to any.
    auto possible = [&](const std::string& from) {
        if (row >= 0) {
            const Row& r = rows_[static_cast<size_t>(row)];
            const int i = paramIndex(r.destination);
            return i >= 0 && allowed(from, r.via == from ? std::string() : r.via,
                                     static_cast<size_t>(i), row);
        }
        for (size_t i = 0; i < m.params.size(); ++i)
            if (allowed(from, {}, i, -1)) return true;
        return false;
    };
    for (bool own : {true, false}) {
        bool any = false;
        for (const SourceMeaning& s : m.sources) {
            if (builtIn(s.key) == own) continue;
            if (!any) {
                items.push_back(gui::MenuItem::header(own ? "Modulators" : "Playing"));
                keys.emplace_back();
                any = true;
            }
            items.push_back(gui::MenuItem::item(
                s.name, row >= 0 && rows_[static_cast<size_t>(row)].source == s.key,
                possible(s.key)));
            keys.push_back(s.key);
        }
    }
    ui_->popup(
        anchor, std::move(items),
        [this, row, anchor, keys](int chosen) {
            if (chosen < 0 || static_cast<size_t>(chosen) >= keys.size()) return;
            const std::string& from = keys[static_cast<size_t>(chosen)];
            if (row < 0) return chooseDestination(-1, anchor, from);
            if (row >= static_cast<int>(rows_.size())) return;
            std::vector<Row> rows = rows_;
            Row& r = rows[static_cast<size_t>(row)];
            r.source = from;
            if (r.via == from) r.via.clear();
            write(std::move(rows));
        },
        180.0f);
}

void RouteList::chooseDestination(int row, gui::Rect anchor, std::string from) {
    std::vector<gui::MenuItem> items;
    std::vector<int> indices;  // each item's parameter, -1 for a header
    const ParamTable& table = definition_.params();
    const Meaning& m = codec_.meaning();
    const std::string via = row >= 0 ? rows_[static_cast<size_t>(row)].via : std::string();
    std::string group = "\x01";  // none yet
    for (size_t i = 0; i < table.count(); ++i) {
        if (m.params[i].routable == Routable::No) continue;
        const ParamDef& p = table.at(i);
        const std::string g = p.group ? p.group : "";
        if (g != group) {
            group = g;
            items.push_back(gui::MenuItem::header(g.empty() ? definition_.info().name : g));
            indices.push_back(-1);
        }
        const bool current =
            row >= 0 && rows_[static_cast<size_t>(row)].destination == m.params[i].key;
        items.push_back(
            gui::MenuItem::item(shortName(i), current, current || allowed(from, via, i, row)));
        indices.push_back(static_cast<int>(i));
    }
    ui_->popup(
        anchor, std::move(items),
        [this, row, indices, from](int chosen) {
            if (chosen < 0 || static_cast<size_t>(chosen) >= indices.size()) return;
            const int index = indices[static_cast<size_t>(chosen)];
            if (index < 0) return;
            const auto i = static_cast<size_t>(index);
            // A new route starts at a tenth of the parameter's own range.
            const double own = routeReach(definition_.params().at(i), ParamMeaning{});
            double start = rounded(i, own * 0.1);
            if (start == 0.0) start = std::pow(10.0, -decimals(i));
            std::vector<Row> rows = rows_;
            if (row < 0) {
                rows.push_back({from, codec_.meaning().params[i].key, {}, start});
                if (write(std::move(rows)))
                    scroll_ = std::max(0, static_cast<int>(rows_.size()) + 1 - visibleRows());
                return;
            }
            if (row >= static_cast<int>(rows.size())) return;
            Row& r = rows[static_cast<size_t>(row)];
            const int before = paramIndex(r.destination);
            const char* was = before >= 0 ? unit(static_cast<size_t>(before)) : nullptr;
            const char* now = unit(i);
            const bool same =
                (was == nullptr) == (now == nullptr) && (!was || std::strcmp(was, now) == 0);
            r.destination = codec_.meaning().params[i].key;
            r.amount = same ? rounded(i, r.amount) : start;
            write(std::move(rows));
        },
        200.0f);
}

void RouteList::chooseVia(int row, gui::Rect anchor) {
    const Row& r = rows_[static_cast<size_t>(row)];
    const int index = paramIndex(r.destination);
    if (index < 0) return;
    std::vector<gui::MenuItem> items;
    std::vector<std::string> keys;
    items.push_back(gui::MenuItem::item("None", r.via.empty()));
    keys.emplace_back();
    items.push_back(gui::MenuItem::separator());
    keys.emplace_back();
    for (const SourceMeaning& s : codec_.meaning().sources) {
        if (s.key == r.source) continue;
        items.push_back(gui::MenuItem::item(
            s.name, r.via == s.key, allowed(r.source, s.key, static_cast<size_t>(index), row)));
        keys.push_back(s.key);
    }
    ui_->popup(
        anchor, std::move(items),
        [this, row, keys](int chosen) {
            if (chosen < 0 || static_cast<size_t>(chosen) >= keys.size() ||
                row >= static_cast<int>(rows_.size()) || chosen == 1)
                return;
            std::vector<Row> rows = rows_;
            rows[static_cast<size_t>(row)].via = keys[static_cast<size_t>(chosen)];
            write(std::move(rows));
        },
        160.0f);
}

void RouteList::rowMenu(int row, float x, float y) {
    std::vector<gui::MenuItem> items{
        gui::MenuItem::item("Type amount\xE2\x80\xA6"), gui::MenuItem::item("Invert amount"),
        gui::MenuItem::separator(), gui::MenuItem::item("Remove route")};
    ui_->popup(
        {x, y, 1.0f, 1.0f}, std::move(items),
        [this, row](int chosen) {
            if (row >= static_cast<int>(rows_.size())) return;
            std::vector<Row> rows = rows_;
            if (chosen == 0) return typeAmount(row);
            if (chosen == 1) rows[static_cast<size_t>(row)].amount *= -1.0;
            if (chosen == 3) rows.erase(rows.begin() + row);
            write(std::move(rows));
        },
        160.0f);
}

void RouteList::typeAmount(int row) {
    const Row& r = rows_[static_cast<size_t>(row)];
    const int index = paramIndex(r.destination);
    if (index < 0) return;
    const gui::Rect at = partRect(row, Part::Amount).inset(-6.0f, -2.0f);
    ui_->prompt(at, amountText(static_cast<size_t>(index), r.amount), 32,
                [this, row](std::string_view typed) {
                    if (row >= static_cast<int>(rows_.size())) return true;
                    const Row& r = rows_[static_cast<size_t>(row)];
                    // Read as a patch reads it: a number in the unit, or
                    // text with a unit that converts.
                    json::Value one = json::Value::object();
                    one[key(r)]["amount"] = std::string(typed);
                    std::string text;
                    std::vector<json::Error> errors;
                    RouteLine line;
                    if (!codec_.routeText(one, "/routes", text, errors) ||
                        !readRouteLine(std::string_view(text).substr(0, text.find('\n')), line))
                        return false;
                    std::vector<Row> rows = rows_;
                    rows[static_cast<size_t>(row)].amount = line.amount;
                    return write(std::move(rows));
                });
}

// --- drawing --------------------------------------------------------------------------------

void RouteList::paint(NVGcontext* vg, const gui::Theme& t) {
    namespace w = gui::widgets;
    // Captions over the columns, and the add button.
    if (!rows_.empty()) {
        const float y = body_.y + 9.0f;
        w::caption(vg, partRect(scroll_, Part::Source).x + 1.0f, y, "Source", gui::rgb(t.textFaint),
                   w::Align::Left, 9.0f);
        w::caption(vg, partRect(scroll_, Part::Destination).x + 1.0f, y, "Moves",
                   gui::rgb(t.textFaint), w::Align::Left, 9.0f);
        w::caption(vg, partRect(scroll_, Part::Amount).cx(), y, "By", gui::rgb(t.textFaint),
                   w::Align::Center, 9.0f);
        w::caption(vg, partRect(scroll_, Part::Via).x + 1.0f, y, "Through", gui::rgb(t.textFaint),
                   w::Align::Left, 9.0f);
    }
    nvgSave(vg);
    nvgIntersectScissor(vg, body_.x - 2.0f, body_.y + kHeader - 2.0f, body_.w + 4.0f,
                        body_.h - kHeader + 4.0f);
    const auto host = [&](size_t k) { return host_->value(definition_.params().at(k).id); };
    const int last = std::min(static_cast<int>(rows_.size()), scroll_ + visibleRows());
    for (int row = scroll_; row < last; ++row) {
        const Row& r = rows_[static_cast<size_t>(row)];
        const gui::Rect box = rowRect(row);
        const bool rowHover = hover_.row == row || dragRow_ == row;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, box.x, box.y, box.w, box.h, 5.0f);
        nvgFillColor(vg, rowHover ? gui::rgb(t.raised) : faded(t.recess, 0.55f));
        nvgFill(vg);
        auto cellHover = [&](Part p) {
            if (hover_.row != row || hover_.part != p || dragRow_ >= 0) return;
            const gui::Rect c = partRect(row, p).inset(0.5f, 2.5f);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, c.x, c.y, c.w, c.h, 4.0f);
            nvgStrokeColor(vg, gui::rgb(t.lineLit));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        };

        // The source: a dot of its kind, its name and a menu's chevron.
        const gui::Rect src = partRect(row, Part::Source);
        cellHover(Part::Source);
        const uint32_t dot = builtIn(r.source) ? t.accent2 : t.modulation;
        nvgBeginPath(vg);
        nvgCircle(vg, src.x + 10.0f, src.cy(), 3.5f);
        nvgFillColor(vg, gui::rgb(dot));
        nvgFill(vg);
        fitted(vg, src.x + 19.0f, src.cy(), sourceName(r.source), 12.0f, src.w - 34.0f,
               gui::font::kUi, gui::rgb(t.text));
        chevron(vg, src.right() - 9.0f, src.cy(), gui::rgb(t.textFaint));

        // The arrow to what it moves.
        const float ax0 = src.right() + 4.0f, ax1 = src.right() + kArrow - 5.0f;
        w::hairline(vg, ax0, src.cy(), ax1, src.cy(), gui::rgb(t.textFaint));
        nvgBeginPath(vg);
        nvgMoveTo(vg, ax1 - 3.5f, src.cy() - 3.0f);
        nvgLineTo(vg, ax1, src.cy());
        nvgLineTo(vg, ax1 - 3.5f, src.cy() + 3.0f);
        nvgStrokeColor(vg, gui::rgb(t.textFaint));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);

        // What it moves, faint while that does not apply.
        const gui::Rect dst = partRect(row, Part::Destination);
        cellHover(Part::Destination);
        const int index = paramIndex(r.destination);
        const bool applies = index < 0 || definition_.applies(static_cast<size_t>(index), host);
        nvgSave(vg);
        if (!applies) nvgGlobalAlpha(vg, 0.45f);
        if (index >= 0) {
            const ParamDef& p = definition_.params().at(static_cast<size_t>(index));
            float x = dst.x + 8.0f;
            const float room = dst.w - 24.0f;
            if (p.group && *p.group) {
                const std::string g = std::string(p.group) + "  ";
                const float gw =
                    std::min(w::textWidth(vg, g.c_str(), 11.0f, gui::font::kUi), room * 0.55f);
                fitted(vg, x, dst.cy(), g, 11.0f, gw, gui::font::kUi, gui::rgb(t.textMuted));
                x += gw;
            }
            fitted(vg, x, dst.cy(), shortName(static_cast<size_t>(index)), 12.0f,
                   dst.x + 8.0f + room - x, gui::font::kUi, gui::rgb(t.text));
        } else {
            fitted(vg, dst.x + 8.0f, dst.cy(), r.destination, 12.0f, dst.w - 24.0f, gui::font::kUi,
                   gui::rgb(t.alert));
        }
        nvgRestore(vg);
        chevron(vg, dst.right() - 9.0f, dst.cy(), gui::rgb(t.textFaint));

        // The amount: a bar from the middle, as far as it reaches.
        const gui::Rect bar = partRect(row, Part::Amount);
        const double most = index >= 0 ? reach(static_cast<size_t>(index)) : 0.0;
        const float share =
            most > 0.0 ? static_cast<float>(std::clamp(r.amount / most, -1.0, 1.0)) : 0.0f;
        w::well(vg, t, bar, 3.0f);
        const float mid = bar.cx();
        const float end = mid + share * (bar.w * 0.5f - 1.0f);
        if (std::fabs(end - mid) > 0.25f) {
            nvgBeginPath(vg);
            nvgRect(vg, std::min(mid, end), bar.y + 1.0f, std::fabs(end - mid), bar.h - 2.0f);
            const bool lit = rowHover && (hover_.part == Part::Amount || dragRow_ == row);
            nvgFillColor(vg, faded(t.modulation, lit ? 0.85f : 0.6f));
            nvgFill(vg);
        }
        w::hairline(vg, mid, bar.y + 1.0f, mid, bar.bottom() - 1.0f, gui::rgb(t.lineLit));
        if (index >= 0)
            w::number(vg, bar.cx(), bar.cy(),
                      amountText(static_cast<size_t>(index), r.amount).c_str(), 10.5f,
                      gui::rgb(r.amount == 0.0 ? t.textMuted : t.text), w::Align::Center);

        // Through: another source's name, or a dash.
        const gui::Rect via = partRect(row, Part::Via);
        cellHover(Part::Via);
        if (r.via.empty()) {
            w::label(vg, via.x + 8.0f, via.cy(), "\xE2\x80\x94", 12.0f, gui::rgb(t.textFaint));
        } else {
            nvgBeginPath(vg);
            nvgCircle(vg, via.x + 10.0f, via.cy(), 3.0f);
            nvgStrokeColor(vg, gui::rgb(builtIn(r.via) ? t.accent2 : t.modulation));
            nvgStrokeWidth(vg, 1.2f);
            nvgStroke(vg);
            fitted(vg, via.x + 19.0f, via.cy(), sourceName(r.via), 11.5f, via.w - 34.0f,
                   gui::font::kUi, gui::rgb(t.textMuted));
        }
        chevron(vg, via.right() - 9.0f, via.cy(), gui::rgb(t.textFaint));

        // Remove, on a row the pointer is over.
        if (rowHover && dragRow_ < 0) {
            const gui::Rect x = partRect(row, Part::Remove);
            const NVGcolor c = gui::rgb(hover_.part == Part::Remove ? t.alert : t.textMuted);
            w::hairline(vg, x.cx() - 3.5f, x.cy() - 3.5f, x.cx() + 3.5f, x.cy() + 3.5f, c, 1.3f);
            w::hairline(vg, x.cx() - 3.5f, x.cy() + 3.5f, x.cx() + 3.5f, x.cy() - 3.5f, c, 1.3f);
        }
    }
    // The row that adds one, outlined; the most a patch holds says so.
    const bool full = rows_.size() >= kMaxRoutes;
    const gui::Rect add = addRect();
    if (static_cast<int>(rows_.size()) < scroll_ + visibleRows()) {
        const bool lit = hover_.part == Part::Add && !full;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, add.x + 0.5f, add.y + 0.5f, add.w - 1.0f, add.h - 1.0f, 5.0f);
        nvgStrokeColor(vg, gui::rgb(lit ? t.lineLit : t.line));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
        if (full) {
            w::label(vg, add.x + 12.0f, add.cy(), "64 routes: the most a patch holds", 11.5f,
                     gui::rgb(t.textFaint));
        } else {
            const NVGcolor c = gui::rgb(lit ? t.text : t.accent);
            w::hairline(vg, add.x + 10.0f, add.cy(), add.x + 18.0f, add.cy(), c, 1.4f);
            w::hairline(vg, add.x + 14.0f, add.cy() - 4.0f, add.x + 14.0f, add.cy() + 4.0f, c,
                        1.4f);
            w::label(vg, add.x + 25.0f, add.cy(), "Add route", 11.5f, c, w::Align::Left, true);
        }
    }
    nvgRestore(vg);

    if (rows_.empty()) {
        const float cy = add.bottom() + (body_.bottom() - add.bottom()) * 0.45f;
        w::label(vg, body_.cx(), cy - 8.0f, "No routes yet", 12.5f, gui::rgb(t.textMuted),
                 w::Align::Center);
        w::label(vg, body_.cx(), cy + 10.0f,
                 "A route moves a setting by a modulator, or by how you play.", 11.0f,
                 gui::rgb(t.textFaint), w::Align::Center);
    }

    // Where the rows shown are among them all.
    const int shown = visibleRows();
    if (static_cast<int>(rows_.size()) + 1 > shown) {
        const float top = body_.y + kHeader, h = body_.bottom() - top;
        const float n = static_cast<float>(rows_.size() + 1);
        const gui::Rect track{body_.right() + 6.0f, top, 3.0f, h};
        nvgBeginPath(vg);
        nvgRoundedRect(vg, track.x, track.y, track.w, track.h, 1.5f);
        nvgFillColor(vg, gui::rgb(t.recess));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, track.x, top + h * static_cast<float>(scroll_) / n, track.w,
                       h * static_cast<float>(shown) / n, 1.5f);
        nvgFillColor(vg, gui::rgb(t.lineLit));
        nvgFill(vg);
    }
}

// --- the pointer ----------------------------------------------------------------------------

bool RouteList::pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) {
    if (!host_ || !ui_) return false;
    const Hit h = hit(x, y);
    hover_ = h;
    if (button == 1) {
        if (h.row < 0) return false;
        rowMenu(h.row, x, y);
        return true;
    }
    if (button != 0) return false;
    switch (h.part) {
        case Part::Add:
            if (rows_.size() < kMaxRoutes) chooseSource(-1, addRect());
            return true;
        case Part::Source:
            chooseSource(h.row, partRect(h.row, Part::Source));
            return true;
        case Part::Destination:
            chooseDestination(h.row, partRect(h.row, Part::Destination),
                              rows_[static_cast<size_t>(h.row)].source);
            return true;
        case Part::Via:
            chooseVia(h.row, partRect(h.row, Part::Via));
            return true;
        case Part::Remove: {
            std::vector<Row> rows = rows_;
            rows.erase(rows.begin() + h.row);
            write(std::move(rows));
            hover_ = hit(x, y);
            return true;
        }
        case Part::Amount: {
            if (doubleClick && (mods & (gui::mod::kCtrl | gui::mod::kSuper))) {
                std::vector<Row> rows = rows_;
                rows[static_cast<size_t>(h.row)].amount = 0.0;
                write(std::move(rows));
                return true;
            }
            if (doubleClick) {
                typeAmount(h.row);
                return true;
            }
            dragRow_ = h.row;
            dragX_ = x;
            dragFrom_ = dragAt_ = rows_[static_cast<size_t>(h.row)].amount;
            fine_ = (mods & gui::mod::kShift) != 0;
            return true;
        }
        default:
            return false;
    }
}

bool RouteList::pointerMove(float x, float y, uint32_t mods) {
    if (dragRow_ >= 0 && dragRow_ < static_cast<int>(rows_.size())) {
        const Row& r = rows_[static_cast<size_t>(dragRow_)];
        const int index = paramIndex(r.destination);
        if (index < 0) return true;
        const auto i = static_cast<size_t>(index);
        // Shift pressed or let go mid-drag: from here, at the new rate.
        const bool fine = (mods & gui::mod::kShift) != 0;
        if (fine != fine_) {
            fine_ = fine;
            dragFrom_ = dragAt_;
            dragX_ = x;
        }
        const float across = std::max(1.0f, partRect(dragRow_, Part::Amount).w);
        dragAt_ = std::clamp(dragFrom_ + static_cast<double>(x - dragX_) / across * 2.0 * reach(i) *
                                             (fine_ ? 0.1 : 1.0),
                             -reach(i), reach(i));
        const double amount = rounded(i, dragAt_);
        if (amount != r.amount) {
            std::vector<Row> rows = rows_;
            rows[static_cast<size_t>(dragRow_)].amount = amount;
            write(std::move(rows));
        }
        return true;
    }
    const Hit h = hit(x, y);
    const bool changed = h.part != hover_.part || h.row != hover_.row;
    hover_ = h;
    return changed;
}

void RouteList::pointerUp(float, float, int, uint32_t) { dragRow_ = -1; }

bool RouteList::wheel(float x, float y, float dx, float dy, uint32_t mods) {
    (void)dx;
    const Hit h = hit(x, y);
    if (h.part == Part::Amount) {
        const Row& r = rows_[static_cast<size_t>(h.row)];
        const int index = paramIndex(r.destination);
        if (index < 0) return true;
        const auto i = static_cast<size_t>(index);
        const double step = std::max(std::pow(10.0, -decimals(i)),
                                     reach(i) * ((mods & gui::mod::kShift) ? 0.001 : 0.01));
        const double amount = rounded(i, r.amount + step * static_cast<double>(dy));
        if (amount != r.amount) {
            std::vector<Row> rows = rows_;
            rows[static_cast<size_t>(h.row)].amount = amount;
            write(std::move(rows));
        }
        return true;
    }
    const int most = std::max(0, static_cast<int>(rows_.size()) + 1 - visibleRows());
    const int before = scroll_;
    scroll_ = std::clamp(scroll_ - static_cast<int>(std::round(dy)), 0, most);
    hover_ = hit(x, y);
    return scroll_ != before || most > 0;
}

void RouteList::pointerLeave() { hover_ = {}; }

void RouteList::cancel() {
    dragRow_ = -1;
    hover_ = {};
}

gui::Cursor RouteList::cursor(float x, float y) const {
    if (dragRow_ >= 0) return gui::Cursor::LeftRight;
    switch (hit(x, y).part) {
        case Part::Amount:
            return gui::Cursor::LeftRight;
        case Part::Source:
        case Part::Destination:
        case Part::Via:
        case Part::Remove:
            return gui::Cursor::Hand;
        case Part::Add:
            return rows_.size() < kMaxRoutes ? gui::Cursor::Hand : gui::Cursor::Arrow;
        default:
            return gui::Cursor::Arrow;
    }
}

}  // namespace supple::suite
