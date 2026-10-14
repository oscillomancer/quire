// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "render_dialog.h"
#include <supple/base/paths.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kUiOrigin{cmd::Origin::Type::Ui, "gui", {}};

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::optional<double> number(std::string_view text) {
    if (text.empty()) return {};
    const std::string s(text);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return {};
    return v;
}

std::optional<std::int64_t> integer(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

RenderDialog::RenderDialog(engine::EngineApp& app, cmd::Dispatcher& dispatcher, ui::Root& root)
    : app_(app), dispatcher_(dispatcher), root_(root) {}

std::string RenderDialog::defaultFor(std::string_view key) const {
    const model::Session& s = app_.head();
    const json::Value& defaults = s.project->render;
    const auto setting = [&](std::string_view k) -> const json::Value* {
        return defaults.isObject() ? defaults.find(k) : nullptr;
    };
    const auto number = [](const json::Value* v, double fallback) {
        return v && v->isNumber() ? v->asDouble() : fallback;
    };
    char buf[64];
    if (key == "format") {  // wav unless the project's render settings say otherwise
        const json::Value* f = setting("format");
        if (f && f->isString()) return f->asString();
        if (f && f->isObject())
            if (const json::Value* c = f->find("container"); c && c->isString())
                return c->asString();
        return "wav";
    }
    if (key == "end_beat") {
        const double last = model::lastItemBeat(s);
        std::snprintf(buf, sizeof buf, "last item end (%.2f)", last);
        return buf;
    }
    if (key == "tail_s") {
        std::snprintf(buf, sizeof buf, "%g", number(setting("tail_ms"), 0.0) / 1000.0);
        return buf;
    }
    if (key == "sample_rate") {
        std::snprintf(buf, sizeof buf, "%d",
                      static_cast<int>(number(setting("sample_rate"), s.project->sampleRate)));
        return buf;
    }
    if (key == "bit_depth") {
        const json::Value* format = setting("format");
        const json::Value* depth =
            format && format->isObject() ? format->find("bit_depth") : setting("bit_depth");
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(number(depth, 32)));
        return buf;
    }
    if (key == "channels") {
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(number(setting("channels"), 2)));
        return buf;
    }
    if (key == "normalize") {
        const json::Value* n = setting("normalize");
        if (n && n->isString()) return n->asString();
        if (n && n->isObject())
            if (const json::Value* mode = n->find("mode"); mode && mode->isString())
                return mode->asString();
        return "none";
    }
    if (key == "target") {
        std::snprintf(buf, sizeof buf, "%g", number(setting("target"), -14.0));
        return buf;
    }
    return {};
}

RenderDialog::~RenderDialog() = default;

std::unique_ptr<ui::Node> RenderDialog::buildPage() {
    auto page = std::make_unique<ui::Node>("render");
    page_ = page.get();
    page->layout.dir = ui::Layout::Dir::Column;
    page->layout.width = ui::SizeRule::fixed(780);
    page->layout.height = ui::SizeRule::fixed(706);
    page->layout.padding = ui::Insets::all(12);
    page->layout.gap = 8;
    auto& header = page->emplace<ui::Node>("render.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(30);
    header.layout.gap = 8;
    header.layout.align = ui::Layout::Align::Center;
    auto& title = header.emplace<ui::Label>("render.title", "Render", ui::Font::UiBold, "title");
    title.layout.width = ui::SizeRule::fraction();
    header.emplace<ui::Button>("render.close", "Close", [this] { close(); });

    // Source: master mix, stems, regions (the command the dialog will call).
    auto& sources = page->emplace<ui::Node>("render.sources");
    sources.layout.dir = ui::Layout::Dir::Row;
    sources.layout.height = ui::SizeRule::fixed(28);
    sources.layout.gap = 6;
    sources.layout.align = ui::Layout::Align::Center;
    sources.emplace<ui::Button>("render.src.master", "Master mix",
                                [this] { setSource(Source::Master); });
    sources.emplace<ui::Button>("render.src.stems", "Stems", [this] { setSource(Source::Stems); });
    sources.emplace<ui::Button>("render.src.regions", "Regions",
                                [this] { setSource(Source::Regions); });
    sourceLabel_ = &sources.emplace<ui::Label>("render.source", "Source: master mix",
                                               ui::Font::Mono, "caption", "ink-quiet");
    sourceLabel_->layout.width = ui::SizeRule::fraction();

    // Where the files go: any folder, chosen in the system's dialog.
    auto& folder = page->emplace<ui::Node>("render.folder.row");
    folder.layout.dir = ui::Layout::Dir::Row;
    folder.layout.height = ui::SizeRule::fixed(28);
    folder.layout.gap = 6;
    folder.layout.align = ui::Layout::Align::Center;
    folder.emplace<ui::Label>("render.folder.label", "Folder", ui::Font::Ui, "caption", "ink-quiet")
        .layout.width = ui::SizeRule::fixed(90);
    folder_ = &folder.emplace<ui::Label>("render.folder", "", ui::Font::Mono, "caption", "ink");
    folder_->layout.width = ui::SizeRule::fraction();
    folder.emplace<ui::Button>("render.folder.choose", "Choose folder…", [this] {
        if (auto choose = onChooseFolder) choose();
    });
    showFolder();

    auto field = [&](const char* label, const char* id, const std::string& value, float width,
                     const std::string& placeholder,
                     std::function<void(const std::string&)> onChange) -> ui::TextField& {
        auto& cell = page->emplace<ui::Node>(std::string(id) + ".cell");
        cell.layout.dir = ui::Layout::Dir::Row;
        cell.layout.height = ui::SizeRule::fixed(26);
        cell.layout.gap = 6;
        cell.layout.align = ui::Layout::Align::Center;
        auto& l = cell.emplace<ui::Label>(std::string(id) + ".label", label, ui::Font::Ui,
                                          "caption", "ink-quiet");
        l.layout.width = ui::SizeRule::fixed(90);
        auto& f = cell.emplace<ui::TextField>(id, value, placeholder);
        f.layout.width = ui::SizeRule::fixed(width);
        f.onChange = [this, onChange = std::move(onChange)](const std::string& text) {
            onChange(text);
            markDirty();
        };
        f.onSubmit = [this](const std::string&) { refreshPreview(); };
        return f;
    };
    field("Pattern", "render.pattern", pattern_, 300, "",
          [this](const std::string& t) { pattern_ = t; });
    field("Start beat", "render.start", start_, 70, "0",
          [this](const std::string& t) { start_ = t; });
    field("End beat", "render.end", end_, 70, defaultFor("end_beat"),
          [this](const std::string& t) { end_ = t; });
    field("Tail (s)", "render.tail", tail_, 70, defaultFor("tail_s"),
          [this](const std::string& t) { tail_ = t; });
    field("Sample rate", "render.sample_rate", sampleRate_, 90, defaultFor("sample_rate"),
          [this](const std::string& t) { sampleRate_ = t; });
    field("Bit depth", "render.bit_depth", bitDepth_, 70, defaultFor("bit_depth"),
          [this](const std::string& t) { bitDepth_ = t; });
    field("Format", "render.format", format_, 70, defaultFor("format"),
          [this](const std::string& t) { format_ = t; });
    field("Second file", "render.secondary", secondary_, 110, "none",
          [this](const std::string& t) { secondary_ = t; });
    field("Channels", "render.channels", channels_, 70, defaultFor("channels"),
          [this](const std::string& t) { channels_ = t; });
    field("Normalize", "render.normalize", normalize_, 110, defaultFor("normalize"),
          [this](const std::string& t) { normalize_ = t; });
    field("Target", "render.target", target_, 80, defaultFor("target"),
          [this](const std::string& t) { target_ = t; });
    field("Limit dBTP", "render.limit", limit_, 80, "-1",
          [this](const std::string& t) { limit_ = t; });

    // Stems-only: how each track is rendered.
    auto& stems = page->emplace<ui::Node>("render.stems.row");
    stemsRow_ = &stems;
    stems.layout.dir = ui::Layout::Dir::Row;
    stems.layout.height = ui::SizeRule::fixed(26);
    stems.layout.gap = 6;
    stems.layout.align = ui::Layout::Align::Center;
    modeLabel_ = &stems.emplace<ui::Label>("render.mode.label", "Mode", ui::Font::Ui, "caption",
                                           "ink-quiet");
    modeLabel_->layout.width = ui::SizeRule::fixed(90);
    modeField_ = &stems.emplace<ui::TextField>("render.mode", mode_);
    modeField_->layout.width = ui::SizeRule::fixed(140);
    modeField_->onChange = [this](const std::string& t) {
        mode_ = t;
        markDirty();
    };
    tapLabel_ =
        &stems.emplace<ui::Label>("render.tap.label", "Tap", ui::Font::Ui, "caption", "ink-quiet");
    tapLabel_->layout.width = ui::SizeRule::fixed(40);
    tapField_ = &stems.emplace<ui::TextField>("render.tap", tap_);
    tapField_->layout.width = ui::SizeRule::fixed(120);
    tapField_->onChange = [this](const std::string& t) {
        tap_ = t;
        markDirty();
    };

    auto& actions = page->emplace<ui::Node>("render.actions");
    actions.layout.dir = ui::Layout::Dir::Row;
    actions.layout.height = ui::SizeRule::fixed(28);
    actions.layout.gap = 6;
    actions.layout.align = ui::Layout::Align::Center;
    actions.emplace<ui::Button>("render.run", "Render", [this] { run(); });
    addButton_ = &actions.emplace<ui::Button>(
        "render.add", addToProject_ ? "[x] Add to project" : "[ ] Add to project",
        [this] { setAddToProject(!addToProject_); });
    actions.emplace<ui::Button>("render.queue.add", "Add to queue", [this] { addToQueue(); });
    queueButton_ = &actions.emplace<ui::Button>("render.queue", "Queue", [this] { openQueue(); });
    status_ =
        &actions.emplace<ui::Label>("render.status", "", ui::Font::Ui, "caption", "ink-quiet");
    status_->layout.width = ui::SizeRule::fraction();

    previewBlock_ =
        &page->emplace<ui::TextBlock>("render.preview", "", ui::Font::Mono, "caption", "ink");
    previewBlock_->layout.height = ui::SizeRule::fixed(60);
    resultsBlock_ =
        &page->emplace<ui::TextBlock>("render.results", "", ui::Font::Mono, "caption", "ink-quiet");
    resultsBlock_->layout.height = ui::SizeRule::fixed(70);
    setSource(Source::Master);
    refreshQueueButton();
    return page;
}

void RenderDialog::open() {
    if (page_) return;
    auto page = buildPage();
    root_.openPopup(std::move(page), {}, ui::Placement::Center, [this] {
        page_ = nullptr;
        sourceLabel_ = nullptr;
        folder_ = nullptr;
        status_ = nullptr;
        previewBlock_ = nullptr;
        resultsBlock_ = nullptr;
        stemsRow_ = nullptr;
        modeLabel_ = nullptr;
        tapLabel_ = nullptr;
        modeField_ = nullptr;
        tapField_ = nullptr;
        addButton_ = nullptr;
        queueButton_ = nullptr;
    });
    dirty_ = true;
    lastEditMs_ = nowMs();
    refreshPreview();
}

void RenderDialog::close() {
    if (page_) root_.closePopup();
}

void RenderDialog::setAddToProject(bool on) {
    addToProject_ = on;
    if (addButton_) addButton_->setText(on ? "[x] Add to project" : "[ ] Add to project");
    root_.damageAll();
}

void RenderDialog::setSource(Source source) {
    source_ = source;
    const char* name = source == Source::Master  ? "master mix"
                       : source == Source::Stems ? "stems"
                                                 : "regions";
    if (sourceLabel_) sourceLabel_->setText(std::string("Source: ") + name);
    if (stemsRow_) {
        stemsRow_->visible = source == Source::Stems;
        if (!stemsRow_->visible) root_.releaseWithin(stemsRow_);
    }
    // Stems-only fields only exist for stems; keep them out of other args.
    if (mode_.empty()) mode_ = "isolated";
    if (tap_.empty()) tap_ = "post_fader";
    if (modeField_) modeField_->setText(mode_);
    if (tapField_) tapField_->setText(tap_);
    markDirty();
    root_.relayout();
    root_.damageAll();
}

void RenderDialog::showFolder() {
    if (!folder_) return;
    std::string text = app_.rendersDir().string();
    if (const auto home = paths::home(); !home.empty()) {
        const std::string prefix = std::string(home) + "/";
        if (text.starts_with(prefix)) text = "~/" + text.substr(prefix.size());
    }
    folder_->setText(text);
    root_.damageAll();
    markDirty();  // the preview's paths change with it
}

const std::string& RenderDialog::folderText() const {
    static const std::string none;
    return folder_ ? folder_->text() : none;
}

const std::string& RenderDialog::pattern() const { return pattern_; }
const std::string& RenderDialog::statusText() const { return statusText_; }

void RenderDialog::setField(const std::string& id, const std::string& text) {
    auto set = [&](std::string& target) {
        target = text;
        if (auto* node = dynamic_cast<ui::TextField*>(root_.find(id))) node->setText(text);
        markDirty();
    };
    if (id == "render.pattern")
        set(pattern_);
    else if (id == "render.start")
        set(start_);
    else if (id == "render.end")
        set(end_);
    else if (id == "render.tail")
        set(tail_);
    else if (id == "render.sample_rate")
        set(sampleRate_);
    else if (id == "render.bit_depth")
        set(bitDepth_);
    else if (id == "render.channels")
        set(channels_);
    else if (id == "render.format")
        set(format_);
    else if (id == "render.secondary")
        set(secondary_);
    else if (id == "render.normalize")
        set(normalize_);
    else if (id == "render.target")
        set(target_);
    else if (id == "render.limit")
        set(limit_);
    else if (id == "render.mode")
        set(mode_);
    else if (id == "render.tap")
        set(tap_);
}

json::Value RenderDialog::previewArgs() const {
    json::Value a = json::Value::object();
    a["operation"] = source_ == Source::Master  ? "mix"
                     : source_ == Source::Stems ? "stems"
                                                : "regions";
    if (!pattern_.empty()) a["pattern"] = pattern_;
    if (auto v = number(start_)) a["start_beat"] = *v;
    if (auto v = number(end_)) a["end_beat"] = *v;
    if (auto v = number(tail_)) a["tail_s"] = *v;
    if (source_ == Source::Stems) {
        if (!mode_.empty()) a["mode"] = mode_;
        if (!tap_.empty()) a["tap"] = tap_;
    }
    outputArgs(a);  // the extension follows the format
    return a;
}

void RenderDialog::outputArgs(json::Value& a) const {
    if (!format_.empty()) a["format"] = format_;
    if (auto v = integer(bitDepth_)) a["bit_depth"] = *v;
    // "flac 16", "wav 24", "flac" (its default depth) or none.
    std::string spec = secondary_;
    while (!spec.empty() && spec.back() == ' ') spec.pop_back();
    if (spec.empty() || spec == "none") return;
    json::Value second = json::Value::object();
    const auto space = spec.find(' ');
    second["format"] = spec.substr(0, space);
    if (space != std::string::npos)
        if (auto v = integer(spec.substr(space + 1))) second["bit_depth"] = *v;
    a["secondary"] = std::move(second);
}

Status RenderDialog::validateFields() const {
    auto invalid = [](const char* field, const char* expected) -> Status {
        return Error("gui.render.field", std::string("render.") + field + ": " + expected,
                     std::string("/") + field,
                     "correct this field or leave it empty for the project default");
    };
    for (const auto& [field, text] : {std::pair{"start", &start_},
                                      {"end", &end_},
                                      {"tail", &tail_},
                                      {"target", &target_},
                                      {"limit", &limit_}}) {
        if (text->empty()) continue;
        const auto value = number(*text);
        if (!value) return invalid(field, "enter a finite number");
        if ((text == &start_ || text == &end_ || text == &tail_) && *value < 0)
            return invalid(field, "enter a non-negative number");
    }
    for (const auto& [field, text] : {std::pair{"sample_rate", &sampleRate_},
                                      {"channels", &channels_},
                                      {"bit_depth", &bitDepth_}}) {
        if (text->empty()) continue;
        const auto value = integer(*text);
        if (!value) return invalid(field, "enter an integer");
        if (text == &sampleRate_ && (*value < 8000 || *value > 384000))
            return invalid(field, "use 8000..384000 Hz");
        if (text == &channels_ && *value != 1 && *value != 2)
            return invalid(field, "use one or two channels");
        if (text == &bitDepth_ && *value != 16 && *value != 24 && *value != 32)
            return invalid(field, "use 16, 24 or 32 bits");
    }
    if (const auto space = secondary_.find(' '); space != std::string::npos) {
        const auto depth = integer(secondary_.substr(space + 1));
        if (!depth || (*depth != 16 && *depth != 24 && *depth != 32))
            return invalid("secondary", "use a format followed by 16, 24 or 32 bits");
    }
    return {};
}

json::Value RenderDialog::args(bool forJob) const {
    json::Value a = json::Value::object();
    if (!pattern_.empty()) a["pattern"] = pattern_;
    if (auto v = number(start_)) a["start_beat"] = *v;
    if (auto v = number(end_)) a["end_beat"] = *v;
    if (auto v = number(tail_)) a["tail_s"] = *v;
    if (auto v = integer(sampleRate_)) a["sample_rate"] = *v;
    outputArgs(a);
    if (auto v = integer(channels_)) a["channels"] = *v;
    if (!normalize_.empty() && normalize_ != "none") {
        json::Value norm = json::Value::object();
        norm["mode"] = normalize_;
        if (auto v = number(target_)) norm["target"] = *v;
        if (auto v = number(limit_)) norm["limit_true_peak_db"] = *v;
        a["normalize"] = std::move(norm);
    }
    if (source_ == Source::Stems) {
        if (!mode_.empty()) a["mode"] = mode_;
        if (!tap_.empty()) a["tap"] = tap_;
    }
    // A mix carries no per-file analysis unless it is asked for; the other
    // commands always attach one, and the dialog lists its loudness.
    if (source_ == Source::Master) a["analyze"] = "summary";
    if (addToProject_) a["add_to_project"] = true;
    if (forJob) a["job"] = true;
    return a;
}

void RenderDialog::markDirty() {
    dirty_ = true;
    lastEditMs_ = nowMs();
}

void RenderDialog::setStatus(std::string text) {
    statusText_ = std::move(text);
    if (status_) status_->setText(statusText_);
}

void RenderDialog::refreshPreview() {
    dirty_ = false;
    if (auto valid = validateFields(); !valid) {
        previewText_ = valid.error().message;
        setStatus(previewText_);
        if (previewBlock_) previewBlock_->setText(previewText_);
        return;
    }
    auto result = dispatcher_.execute({"render.dry-run", previewArgs(), kUiOrigin});
    if (!result) {
        previewText_ = "pattern: " + result.error().message;
        setStatus(result.error().message);
        if (previewBlock_) previewBlock_->setText(previewText_);
        return;
    }
    const json::Value* files = result->find("files");
    std::string text;
    std::size_t count = 0;
    if (files && files->isArray())
        for (const json::Value& f : files->asArray()) {
            const std::string path =
                f.find("path") && f.find("path")->isString() ? f.find("path")->asString() : "";
            const double seconds = f.find("seconds") && f.find("seconds")->isNumber()
                                       ? f.find("seconds")->asDouble()
                                       : 0.0;
            char line[256];
            std::snprintf(line, sizeof line, "%s  (%.2f s)", baseName(path).c_str(), seconds);
            text += (text.empty() ? "" : "\n") + std::string(line);
            ++count;
        }
    if (count == 0) text = "no files";
    previewText_ = text;
    setStatus(std::to_string(count) + (count == 1 ? " file" : " files"));
    if (previewBlock_) previewBlock_->setText(previewText_);
    root_.damageAll();
}

void RenderDialog::run() {
    if (!job_.empty()) {
        setStatus("a render job is already active; wait for it to finish");
        return;
    }
    if (auto valid = validateFields(); !valid) {
        setStatus(valid.error().message);
        return;
    }
    const char* command = source_ == Source::Master  ? "render.mix"
                          : source_ == Source::Stems ? "render.stems"
                                                     : "render.regions";
    auto result = dispatcher_.execute({command, args(true), kUiOrigin});
    if (!result) {
        setStatus(std::string(command) + ": " + result.error().message);
        return;
    }
    if (const json::Value* id = result->find("job"); id && id->isString()) {
        job_ = id->asString();
        resultFiles_.clear();
        setStatus("rendering…");
        if (resultsBlock_) resultsBlock_->setText("");
    } else {
        // A non-job result (should not happen with job:true) is shown directly.
        job_.clear();
        setStatus("done");
        if (resultsBlock_) resultsBlock_->setText(json::writeCompact(*result));
    }
}

void RenderDialog::addToQueue() {
    if (auto valid = validateFields(); !valid) {
        setStatus(valid.error().message);
        return;
    }
    const char* command = source_ == Source::Master  ? "render.mix"
                          : source_ == Source::Stems ? "render.stems"
                                                     : "render.regions";
    json::Value a = args(false);
    if (json::Object* o = a.isObject() ? &a.asObject() : nullptr) o->erase("analyze");
    json::Value q = json::Value::object();
    q["command"] = command;
    q["args"] = std::move(a);
    auto result = dispatcher_.execute({"render.queue.add", std::move(q), kUiOrigin});
    if (!result) {
        setStatus("queue: " + result.error().message);
        return;
    }
    const json::Value* label = result->find("label");
    setStatus("queued " + (label && label->isString() ? label->asString() : std::string()));
    refreshQueueButton();
}

void RenderDialog::refreshQueueButton() {
    if (!queueButton_) return;
    const std::size_t n = app_.renderQueue().size();
    queueButton_->setText(n ? "Queue (" + std::to_string(n) + ")" : "Queue");
}

void RenderDialog::openQueue() {
    auto panel = std::make_unique<ui::Node>("render.queue.panel");
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.width = ui::SizeRule::fixed(520);
    panel->layout.padding = ui::Insets::all(12);
    panel->layout.gap = 8;
    panel->emplace<ui::Label>("render.queue.title", "Render queue", ui::Font::UiBold, "title");
    std::string text;
    for (const auto& q : app_.renderQueue()) {
        text += (text.empty() ? "" : "\n") + q.id + "  " + q.label;
        if (q.files.isArray())
            for (const json::Value& f : q.files.asArray())
                if (f.isString()) text += "\n    " + baseName(f.asString());
    }
    auto& list = panel->emplace<ui::TextBlock>(
        "render.queue.list", text.empty() ? "Nothing queued: Add to queue sets one up." : text,
        ui::Font::Mono, "caption", "ink");
    list.layout.height = ui::SizeRule::fixed(180);
    auto& row = panel->emplace<ui::Node>("render.queue.buttons");
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(26);
    row.layout.gap = 6;
    auto& go = row.emplace<ui::Button>("render.queue.run", "Render all", [this] {
        root_.closePopup();
        runQueue();
    });
    go.enabled = !app_.renderQueue().empty();
    row.emplace<ui::Button>("render.queue.clear", "Clear", [this] {
        json::Value a = json::Value::object();
        a["all"] = true;
        (void)dispatcher_.execute({"render.queue.remove", std::move(a), kUiOrigin});
        root_.closePopup();
        refreshQueueButton();
        setStatus("queue cleared");
    });
    row.emplace<ui::Button>("render.queue.close", "Close", [this] { root_.closePopup(); });
    root_.openPopup(std::move(panel), {}, ui::Placement::Center);
}

void RenderDialog::runQueue() {
    if (!job_.empty()) {
        setStatus("a render job is already active; wait for it to finish");
        return;
    }
    json::Value a = json::Value::object();
    a["job"] = true;
    auto result = dispatcher_.execute({"render.queue.run", std::move(a), kUiOrigin});
    if (!result) {
        setStatus("queue: " + result.error().message);
        return;
    }
    if (const json::Value* id = result->find("job"); id && id->isString()) {
        job_ = id->asString();
        resultFiles_.clear();
        setStatus("rendering the queue…");
        if (resultsBlock_) resultsBlock_->setText("");
    }
}

void RenderDialog::pollJob() {
    if (job_.empty()) return;
    json::Value argsJson = json::Value::object();
    argsJson["job"] = job_;
    auto status = dispatcher_.execute({"job.get", std::move(argsJson), kUiOrigin});
    if (!status) {
        setStatus(status.error().message);
        job_.clear();
        return;
    }
    const json::Value* state = status->find("status");
    const std::string s = state && state->isString() ? state->asString() : "";
    if (s == "running") {
        const json::Value* progress = status->find("progress");
        const json::Value* fraction = progress ? progress->find("fraction") : nullptr;
        if (fraction && fraction->isNumber()) {
            char text[48];
            std::snprintf(text, sizeof text, "rendering… %3.0f%%", 100 * fraction->asDouble());
            setStatus(text);
        }
        return;
    }
    job_.clear();
    if (s == "failed") {
        const json::Value* error = status->find("error");
        const json::Value* message = error ? error->find("message") : nullptr;
        setStatus(message && message->isString() ? message->asString() : "render failed");
        return;
    }
    // done: list the files (render.mix returns one file, stems/regions a list).
    resultFiles_.clear();
    std::string text;
    auto addFile = [&](const json::Value& entry) {
        const std::string path = entry.find("path") && entry.find("path")->isString()
                                     ? entry.find("path")->asString()
                                     : "";
        const double seconds = entry.find("seconds") && entry.find("seconds")->isNumber()
                                   ? entry.find("seconds")->asDouble()
                                   : 0.0;
        resultFiles_.push_back(path);
        char line[320];
        std::snprintf(line, sizeof line, "%s  (%.2f s)", baseName(path).c_str(), seconds);
        std::string row = line;
        // The analysis summary, when the render carries one: loudness and peak.
        if (const json::Value* analysis = entry.find("analysis");
            analysis && analysis->isObject()) {
            const json::Value* loudness = analysis->find("loudness");
            if (loudness && loudness->isObject()) {
                const auto append = [&](const char* key, const char* unit) {
                    const json::Value* v = loudness->find(key);
                    if (!v || !v->isNumber()) return;
                    char part[48];
                    std::snprintf(part, sizeof part, "  %.1f %s", v->asDouble(), unit);
                    row += part;
                };
                append("integrated_lufs", "LUFS");
                append("true_peak_dbtp", "dBTP");
            }
        }
        text += (text.empty() ? "" : "\n") + row;
    };
    const json::Value* result = status->find("result");
    if (result && result->isObject()) {
        if (const json::Value* renders = result->find("renders"); renders && renders->isArray()) {
            for (const json::Value& r : renders->asArray())  // the queue: each render's files
                if (const json::Value* files = r.find("files"); files && files->isArray())
                    for (const json::Value& f : files->asArray()) addFile(f);
        } else if (const json::Value* files = result->find("files"); files && files->isArray())
            for (const json::Value& f : files->asArray()) addFile(f);
        else if (result->find("path"))
            addFile(*result);
    }
    const bool cancelled = s == "cancelled" || s == "canceled";
    setStatus((cancelled ? "cancelled; " : "") + std::to_string(resultFiles_.size()) +
              (resultFiles_.size() == 1 ? " file rendered" : " files rendered"));
    refreshQueueButton();
    if (resultsBlock_) resultsBlock_->setText(text.empty() ? "no files" : text);
    root_.damageAll();
}

void RenderDialog::tick() {
    if (!page_) return;
    if (!job_.empty()) pollJob();
    if (dirty_ && nowMs() - lastEditMs_ >= static_cast<std::uint64_t>(debounceMs_))
        refreshPreview();
}

std::optional<std::chrono::steady_clock::time_point> RenderDialog::nextDeadline() const {
    if (!page_ || !dirty_) return {};
    return std::chrono::steady_clock::time_point(
        std::chrono::milliseconds(lastEditMs_ + std::max(0, debounceMs_)));
}

}  // namespace supple::gui
