#include "ui_persistence.h"

#include <cstdio>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"

namespace supple::gui {

void UiPersistence::add(std::string section, Save save, Load load) {
    sections_.push_back({std::move(section), std::move(save), std::move(load)});
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
    for (const Section& s : sections_)
        if (const json::Value* v = doc.isObject() ? doc.find(s.name) : nullptr) s.load(*v);
}

void UiPersistence::tick(const std::filesystem::path& project) {
    if (project != current_) {  // opened, saved under a new name, or closed
        current_ = project;
        written_.clear();
        const auto file = fileFor(project);
        if (!file.empty() && std::filesystem::exists(file)) {
            if (auto bytes = readFile(file)) {
                const std::string text(bytes->begin(), bytes->end());
                if (auto doc = json::parse(text); doc && doc->isObject()) {
                    apply(*doc);
                    written_ = json::writeCanonical(collect());
                } else {
                    std::fprintf(stderr, "ui: %s is not valid UI state; ignored\n", file.c_str());
                }
            }
        }
        nextCheck_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (project.empty() || now < nextCheck_) return;
    nextCheck_ = now + std::chrono::seconds(1);
    (void)flush(project);
}

Status UiPersistence::flush(const std::filesystem::path& project) {
    const auto file = fileFor(project);
    if (file.empty()) return {};
    std::string text = json::writeCanonical(collect());
    if (text == written_) return {};
    auto w = writeFileAtomic(file, text + "\n");
    if (w) written_ = std::move(text);
    return w;
}

}  // namespace supple::gui
