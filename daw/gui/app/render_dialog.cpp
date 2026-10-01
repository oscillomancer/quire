#include "render_dialog.h"

#include <algorithm>
#include <chrono>
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
    if (end == s.c_str() || *end != '\0') return {};
    return v;
}

std::optional<std::int64_t> integer(std::string_view text) {
    auto v = number(text);
    return v ? std::optional<std::int64_t>(static_cast<std::int64_t>(*v)) : std::nullopt;
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

RenderDialog::RenderDialog(engine::EngineApp&, cmd::Dispatcher& dispatcher, ui::Root& root)
    : dispatcher_(dispatcher), root_(root) {}

RenderDialog::~RenderDialog() = default;

std::unique_ptr<ui::Node> RenderDialog::buildPage() {
    auto page = std::make_unique<ui::Node>("render");
    page_ = page.get();
    page->layout.dir = ui::Layout::Dir::Column;
    page->layout.width = ui::SizeRule::fixed(780);
    page->layout.height = ui::SizeRule::fixed(600);
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

    auto field = [&](const char* label, const char* id, const std::string& value, float width,
                     std::function<void(const std::string&)> onChange) -> ui::TextField& {
        auto& cell = page->emplace<ui::Node>(std::string(id) + ".cell");
        cell.layout.dir = ui::Layout::Dir::Row;
        cell.layout.height = ui::SizeRule::fixed(26);
        cell.layout.gap = 6;
        cell.layout.align = ui::Layout::Align::Center;
        auto& l = cell.emplace<ui::Label>(std::string(id) + ".label", label, ui::Font::Ui,
                                          "caption", "ink-quiet");
        l.layout.width = ui::SizeRule::fixed(90);
        auto& f = cell.emplace<ui::TextField>(id, value);
        f.layout.width = ui::SizeRule::fixed(width);
        f.onChange = [this, onChange = std::move(onChange)](const std::string& text) {
            onChange(text);
            markDirty();
        };
        f.onSubmit = [this](const std::string&) { refreshPreview(); };
        return f;
    };
    field("Pattern", "render.pattern", pattern_, 300,
          [this](const std::string& t) { pattern_ = t; });
    field("Start beat", "render.start", start_, 70, [this](const std::string& t) { start_ = t; });
    field("End beat", "render.end", end_, 70, [this](const std::string& t) { end_ = t; });
    field("Tail (s)", "render.tail", tail_, 70, [this](const std::string& t) { tail_ = t; });
    field("Sample rate", "render.sample_rate", sampleRate_, 90,
          [this](const std::string& t) { sampleRate_ = t; });
    field("Bit depth", "render.bit_depth", bitDepth_, 70,
          [this](const std::string& t) { bitDepth_ = t; });
    field("Channels", "render.channels", channels_, 70,
          [this](const std::string& t) { channels_ = t; });
    field("Normalize", "render.normalize", normalize_, 110,
          [this](const std::string& t) { normalize_ = t; });
    field("Target", "render.target", target_, 80, [this](const std::string& t) { target_ = t; });
    field("Limit dBTP", "render.limit", limit_, 80, [this](const std::string& t) { limit_ = t; });

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
    tapLabel_ = &stems.emplace<ui::Label>("render.tap.label", "Tap", ui::Font::Ui, "caption",
                                          "ink-quiet");
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
    status_ = &actions.emplace<ui::Label>("render.status", "", ui::Font::Ui, "caption", "ink-quiet");
    status_->layout.width = ui::SizeRule::fraction();

    previewBlock_ = &page->emplace<ui::TextBlock>("render.preview", "", ui::Font::Mono, "caption",
                                                  "ink");
    previewBlock_->layout.height = ui::SizeRule::fixed(60);
    resultsBlock_ = &page->emplace<ui::TextBlock>("render.results", "", ui::Font::Mono, "caption",
                                                  "ink-quiet");
    resultsBlock_->layout.height = ui::SizeRule::fixed(70);
    setSource(Source::Master);
    return page;
}

void RenderDialog::open() {
    if (page_) return;
    auto page = buildPage();
    root_.openPopup(std::move(page), {}, ui::Placement::Center, [this] {
        page_ = nullptr;
        sourceLabel_ = nullptr;
        status_ = nullptr;
        previewBlock_ = nullptr;
        resultsBlock_ = nullptr;
        stemsRow_ = nullptr;
        modeLabel_ = nullptr;
        tapLabel_ = nullptr;
        modeField_ = nullptr;
        tapField_ = nullptr;
    });
    dirty_ = true;
    lastEditMs_ = nowMs();
    refreshPreview();
}

void RenderDialog::close() {
    if (page_) root_.closePopup();
}

void RenderDialog::setSource(Source source) {
    source_ = source;
    const char* name = source == Source::Master  ? "master mix"
                       : source == Source::Stems ? "stems"
                                                 : "regions";
    if (sourceLabel_) sourceLabel_->setText(std::string("Source: ") + name);
    if (stemsRow_) stemsRow_->visible = source == Source::Stems;
    // Stems-only fields only exist for stems; keep them out of other args.
    if (mode_.empty()) mode_ = "isolated";
    if (tap_.empty()) tap_ = "post_fader";
    if (modeField_) modeField_->setText(mode_);
    if (tapField_) tapField_->setText(tap_);
    markDirty();
    root_.relayout();
    root_.damageAll();
}

const std::string& RenderDialog::pattern() const { return pattern_; }
const std::string& RenderDialog::statusText() const { return statusText_; }

void RenderDialog::setField(const std::string& id, const std::string& text) {
    auto set = [&](std::string& target) {
        target = text;
        if (auto* node = dynamic_cast<ui::TextField*>(root_.find(id))) node->setText(text);
        markDirty();
    };
    if (id == "render.pattern") set(pattern_);
    else if (id == "render.start") set(start_);
    else if (id == "render.end") set(end_);
    else if (id == "render.tail") set(tail_);
    else if (id == "render.sample_rate") set(sampleRate_);
    else if (id == "render.bit_depth") set(bitDepth_);
    else if (id == "render.channels") set(channels_);
    else if (id == "render.normalize") set(normalize_);
    else if (id == "render.target") set(target_);
    else if (id == "render.limit") set(limit_);
    else if (id == "render.mode") set(mode_);
    else if (id == "render.tap") set(tap_);
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
    return a;
}

json::Value RenderDialog::args(bool forJob) const {
    json::Value a = json::Value::object();
    if (!pattern_.empty()) a["pattern"] = pattern_;
    if (auto v = number(start_)) a["start_beat"] = *v;
    if (auto v = number(end_)) a["end_beat"] = *v;
    if (auto v = number(tail_)) a["tail_s"] = *v;
    if (auto v = integer(sampleRate_)) a["sample_rate"] = *v;
    if (auto v = integer(bitDepth_)) a["bit_depth"] = *v;
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
            const std::string path = f.find("path") && f.find("path")->isString()
                                         ? f.find("path")->asString()
                                         : "";
            const double seconds =
                f.find("seconds") && f.find("seconds")->isNumber() ? f.find("seconds")->asDouble()
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
        const double seconds =
            entry.find("seconds") && entry.find("seconds")->isNumber() ? entry.find("seconds")->asDouble()
                                                                       : 0.0;
        resultFiles_.push_back(path);
        char line[256];
        std::snprintf(line, sizeof line, "%s  (%.2f s)", baseName(path).c_str(), seconds);
        text += (text.empty() ? "" : "\n") + std::string(line);
    };
    const json::Value* result = status->find("result");
    if (result && result->isObject()) {
        if (const json::Value* files = result->find("files"); files && files->isArray())
            for (const json::Value& f : files->asArray()) addFile(f);
        else if (result->find("path"))
            addFile(*result);
    }
    setStatus(std::to_string(resultFiles_.size()) +
              (resultFiles_.size() == 1 ? " file rendered" : " files rendered"));
    if (resultsBlock_) resultsBlock_->setText(text.empty() ? "no files" : text);
    root_.damageAll();
}

void RenderDialog::tick() {
    if (!page_) return;
    if (!job_.empty()) pollJob();
    if (dirty_ && nowMs() - lastEditMs_ >= static_cast<std::uint64_t>(debounceMs_)) refreshPreview();
}

}  // namespace supple::gui
