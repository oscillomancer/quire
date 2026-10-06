#include "preferences.h"

#include <cstdlib>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"
#include "supple/ui/popup.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

constexpr const char* kSchema = "supple.preferences/1.0";

const char* monitorName(model::Monitor m) {
    return m == model::Monitor::Off ? "off" : m == model::Monitor::On ? "on" : "auto";
}

std::string monitorLabel(model::Monitor m) {
    switch (m) {
        case model::Monitor::Off:
            return "Off";
        case model::Monitor::On:
            return "On (while armed)";
        case model::Monitor::Auto:
            return "Auto (armed: stopped or recording)";
    }
    return "On";
}

std::string zoomLabel(std::string_view b) {
    return b == "mouse_pointer" ? "The mouse pointer" : "The project cursor";
}

ui::Node& row(ui::Node& form, const std::string& id, const std::string& label) {
    auto& r = form.emplace<ui::Node>(id);
    r.layout.dir = ui::Layout::Dir::Row;
    r.layout.height = ui::SizeRule::fixed(26);
    r.layout.gap = 8;
    r.layout.align = ui::Layout::Align::Center;
    r.emplace<ui::Label>(id + ".label", label, ui::Font::Ui, "small", "ink-quiet").layout.width =
        ui::SizeRule::fixed(200);
    return r;
}

}  // namespace

std::filesystem::path Preferences::defaultFile(std::string_view slug) {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / slug / "preferences.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / slug / "preferences.json";
    return {};
}

Result<Preferences> Preferences::load(const std::filesystem::path& file) {
    Preferences out;
    if (file.empty() || !std::filesystem::exists(file)) return out;
    auto bytes = readFile(file);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    const json::Value* schema = doc->isObject() ? doc->find("schema") : nullptr;
    if (!schema || !schema->isString() || schema->asString() != kSchema)
        return Error("preferences.schema", file.string() + " is not a " + kSchema + " document",
                     "/schema");
    if (const json::Value* m = doc->find("default_monitor")) {
        const std::string v = m->isString() ? m->asString() : std::string();
        if (v == "off")
            out.defaultMonitor = model::Monitor::Off;
        else if (v == "on")
            out.defaultMonitor = model::Monitor::On;
        else if (v == "auto")
            out.defaultMonitor = model::Monitor::Auto;
        else
            return Error("preferences.default_monitor", "default_monitor is off, on or auto",
                         "/default_monitor");
    }
    return out;
}

Status Preferences::save(const std::filesystem::path& file) const {
    json::Value doc = json::Value::object();
    doc["schema"] = kSchema;
    doc["default_monitor"] = monitorName(defaultMonitor);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return writeFileAtomic(file, json::writeCanonical(doc) + "\n");
}

PreferencesDialog::PreferencesDialog(ui::Root& root, engine::EngineApp& app, MouseDialog& mouse,
                                     views::MouseMap& map, Preferences prefs,
                                     std::filesystem::path file)
    : root_(root), app_(app), mouse_(mouse), map_(map), prefs_(prefs), file_(std::move(file)) {
    app_.setDefaultMonitor(prefs_.defaultMonitor);
}

bool PreferencesDialog::isOpen() const { return root_.find("prefs") != nullptr; }

void PreferencesDialog::open() {
    if (root_.popupOpen()) root_.closePopups();
    auto form = std::make_unique<ui::Node>("prefs");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(500);
    form->layout.height = ui::SizeRule::fixed(150);
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 6;
    auto& header = form->emplace<ui::Node>("prefs.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(28);
    header.layout.align = ui::Layout::Align::Center;
    header.emplace<ui::Label>("prefs.title", "Preferences", ui::Font::UiBold, "body").layout.width =
        ui::SizeRule::fraction();
    header.emplace<ui::Button>("prefs.close", "Close", [this] { root_.closePopups(); });

    auto& monitor = row(*form, "prefs.monitor", "New channels' monitoring");
    monitor_ = &monitor.emplace<ui::Button>("prefs.monitor.set", "");
    monitor_->layout.width = ui::SizeRule::fraction();
    monitor_->layout.height = ui::SizeRule::fixed(26);
    monitor_->onClick = [this] {
        std::vector<ui::MenuItem> items;
        for (model::Monitor m : {model::Monitor::Off, model::Monitor::On, model::Monitor::Auto}) {
            ui::MenuItem item;
            item.label = monitorLabel(m);
            item.checked = prefs_.defaultMonitor == m;
            item.run = [this, m] { setDefaultMonitor(m); };
            items.push_back(std::move(item));
        }
        root_.openPopup(
            ui::makeMenu("prefs.monitor.menu", std::move(items), [this] { root_.closePopup(); }),
            monitor_->bounds());
    };

    auto& zoom = row(*form, "prefs.zoom", "Wheel zoom anchors at");
    zoom_ = &zoom.emplace<ui::Button>("prefs.zoom.set", "");
    zoom_->layout.width = ui::SizeRule::fraction();
    zoom_->layout.height = ui::SizeRule::fixed(26);
    zoom_->onClick = [this] {
        std::vector<ui::MenuItem> items;
        for (const char* b : {"project_cursor", "mouse_pointer"}) {
            ui::MenuItem item;
            item.label = zoomLabel(b);
            item.checked = map_.behaviour("wheel.zoom", 0) == b;
            item.run = [this, value = std::string(b)] { setZoomAnchor(value); };
            items.push_back(std::move(item));
        }
        root_.openPopup(
            ui::makeMenu("prefs.zoom.menu", std::move(items), [this] { root_.closePopup(); }),
            zoom_->bounds());
    };

    auto& mouse = row(*form, "prefs.mouse", "Mouse modifiers");
    mouse
        .emplace<ui::Button>("prefs.mouse.open", "Edit mouse modifiers…", [this] { mouse_.open(); })
        .layout.height = ui::SizeRule::fixed(26);
    root_.openPopup(std::move(form), {}, ui::Placement::Center, [this] {
        monitor_ = nullptr;
        zoom_ = nullptr;
    });
    refresh();
}

void PreferencesDialog::refresh() {
    if (monitor_) monitor_->setText(monitorLabel(prefs_.defaultMonitor));
    if (zoom_) zoom_->setText(zoomLabel(map_.behaviour("wheel.zoom", 0)));
}

void PreferencesDialog::setDefaultMonitor(model::Monitor m) {
    prefs_.defaultMonitor = m;
    app_.setDefaultMonitor(m);
    if (!file_.empty())
        if (auto saved = prefs_.save(file_); !saved && onError)
            onError("preferences: " + saved.error().toString());
    refresh();
}

void PreferencesDialog::setZoomAnchor(const std::string& behaviour) {
    mouse_.set("wheel.zoom", 0, behaviour);  // the mouse map's own file keeps it
    refresh();
}

}  // namespace supple::gui
