// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "ui_persistence.h"
#include "supple/base/stop.h"

#include <cstdio>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"

namespace supple::gui {

UiPersistence::UiPersistence(std::function<void()> completed)
    : completed_(std::move(completed)),
      writer_([this](supple::StopToken stop) { writerLoop(stop); }) {}

UiPersistence::~UiPersistence() {
    drain();
    {
        std::lock_guard lock(mutex_);
        writer_.request_stop();
    }
    wake_.notify_all();
}

void UiPersistence::writerLoop(supple::StopToken stop) {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stop.stop_requested() || pending_.has_value(); });
        if (!pending_) return;  // stopped
        auto [file, text] = std::move(*pending_);
        pending_.reset();
        writing_ = true;
        lock.unlock();
        if (auto w = writeFileAtomic(file, text + "\n"); !w) {
            std::fprintf(stderr, "ui: %s\n", w.error().message.c_str());
            writeFailed_ = true;
        }
        lock.lock();
        writing_ = false;
        wake_.notify_all();
        lock.unlock();
        if (completed_) completed_();
        lock.lock();
    }
}

void UiPersistence::drain() {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [&] { return !pending_ && !writing_; });
}

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

void UiPersistence::arm(std::chrono::steady_clock::time_point now) {
    if (!nextCheck_) nextCheck_ = now + std::chrono::seconds(1);
}

void UiPersistence::tick(const std::filesystem::path& project, std::uint64_t generation,
                         std::chrono::steady_clock::time_point now) {
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
                                 file.string().c_str());
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
        nextCheck_.reset();
        if (!project.empty() && !protectedFile_) arm(now);
        return;
    }
    prepared_ = false;  // a refused project command did not replace anything
    if (writeFailed_.exchange(false)) {
        written_.clear();
        arm(now);  // bounded retry, woken by the writer's completion
    }
    if (!nextCheck_ || now < *nextCheck_) return;
    nextCheck_.reset();
    const auto file = fileFor(project);
    if (file.empty() || protectedFile_) return;
    std::string text = json::writeCanonical(collect());
    if (text == written_) return;
    written_ = text;
    {
        std::lock_guard lock(mutex_);
        pending_.emplace(file, std::move(text));
    }
    wake_.notify_all();
}

Status UiPersistence::flush(const std::filesystem::path& project) {
    drain();  // a pending background write must not land after this one
    nextCheck_.reset();
    if (writeFailed_.exchange(false)) written_.clear();
    const auto file = fileFor(project);
    if (file.empty() || (project == current_ && protectedFile_)) return {};
    std::string text = json::writeCanonical(collect());
    if (text == written_) return {};
    auto w = writeFileAtomic(file, text + "\n");
    if (w) written_ = std::move(text);
    return w;
}

}  // namespace supple::gui
