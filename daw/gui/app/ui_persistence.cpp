#include "ui_persistence.h"

#include <cstdio>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"

namespace supple::gui {

void UiPersistence::add(std::string section, Save save, Load load) {
    auto defaults = save();
    sections_.push_back(
        {std::move(section), std::move(save), std::move(load), std::move(defaults)});
}

std::filesystem::path UiPersistence::fileFor(const std::filesystem::path& project) {
    if (project.empty()) return {};
    return project.parent_path() / (project.stem().string() + ".ui.json");
}

json::Value UiPersistence::collect() const {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.ui/1.0";
    for (const Section& s : sections_) doc[s.name] = s.save();
    return doc;
}

void UiPersistence::apply(const json::Value& doc) {
    for (const Section& s : sections_) {
        s.load(s.defaults);
        if (const json::Value* v = doc.isObject() ? doc.find(s.name) : nullptr) s.load(*v);
    }
}

void UiPersistence::prepareProjectSwitch() {
    if (!initialized_ || prepared_) return;
    if (auto status = flush(current_); !status)
        std::fprintf(stderr, "ui: %s\n", status.error().message.c_str());
    prepared_ = true;
}

void UiPersistence::tick(const std::filesystem::path& project) {
    // Path-only callers treat each path change as an open. The app supplies
    // the engine generation so Save As can preserve the current layout.
    tick(project, initialized_ && project != current_ ? generation_ + 1 : generation_);
}

void UiPersistence::tick(const std::filesystem::path& project, std::uint64_t generation) {
    const bool replaced = !initialized_ || generation != generation_;
    if (replaced || project != current_) {
        if (initialized_ && !prepared_) prepareProjectSwitch();
        current_ = project;
        generation_ = generation;
        initialized_ = true;
        prepared_ = false;
        protectedFile_ = false;
        written_.clear();
        json::Value state = json::Value::object();
        const auto file = fileFor(project);
        if (!file.empty() && std::filesystem::exists(file)) {
            if (auto bytes = readFile(file)) {
                const std::string text(bytes->begin(), bytes->end());
                auto doc = json::parse(text);
                // A missing schema reads as 1.x (hand-written files); another
                // major is left alone.
                const auto* schema = doc && doc->isObject() ? doc->find("schema") : nullptr;
                const bool supported =
                    doc && doc->isObject() &&
                    (!schema ||
                     (schema->isString() && schema->asString().starts_with("supple.ui/1.") &&
                      schema->asString().size() > 12 &&
                      schema->asString().find_first_not_of("0123456789", 12) == std::string::npos));
                if (supported) {
                    state = *doc;
                } else {
                    protectedFile_ = true;
                    std::fprintf(stderr, "ui: %s has invalid or unsupported UI state; ignored\n",
                                 file.c_str());
                }
            } else {
                protectedFile_ = true;  // unreadable state must not be overwritten
            }
        }
        if (replaced) apply(state);
        // Save As copies the current layout, even if the target has a sidecar.
        // Unsupported existing files stay untouched in either case.
        if (replaced && !protectedFile_ && !state.asObject().empty())
            written_ = json::writeCanonical(collect());
        nextCheck_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        return;
    }
    prepared_ = false;  // a refused project command did not replace anything
    const auto now = std::chrono::steady_clock::now();
    if (project.empty() || now < nextCheck_) return;
    nextCheck_ = now + std::chrono::seconds(1);
    (void)flush(project);
}

Status UiPersistence::flush(const std::filesystem::path& project) {
    const auto file = fileFor(project);
    if (file.empty() || (project == current_ && protectedFile_)) return {};
    std::string text = json::writeCanonical(collect());
    if (text == written_) return {};
    auto w = writeFileAtomic(file, text + "\n");
    if (w) written_ = std::move(text);
    return w;
}

}  // namespace supple::gui
