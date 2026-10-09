// Per-project UI state (docs/specs/gui.md §1.2): zoom, scroll, selection,
// panels and window size live in <project stem>.ui.json beside the project
// (canonical JSON, never in the project or its diff). Views register a
// section each; the file is read on project replacement and written
// (at most once a second) when the serialized state differs from the file.
// Input/commands arm those writes on a writer thread, so a slow disk never stalls
// the UI; flush() writes synchronously, after any pending write.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/value.h"

namespace supple::gui {

class UiPersistence {
  public:
    explicit UiPersistence(std::function<void()> completed = {});
    ~UiPersistence();  // finishes a pending write
    UiPersistence(const UiPersistence&) = delete;
    UiPersistence& operator=(const UiPersistence&) = delete;
    using Save = std::function<json::Value()>;
    using Load = std::function<void(const json::Value&)>;
    void add(std::string section, Save save, Load load);

    // Call on service/input/deadlines with the current path (empty: unsaved).
    void tick(const std::filesystem::path& project);
    void tick(const std::filesystem::path& project, std::uint64_t generation,
              std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // Input/commands coalesce into one check about a second later. Continued
    // input keeps the earliest deadline, so a drag cannot postpone writes forever.
    void arm(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::optional<std::chrono::steady_clock::time_point> nextDeadline() const { return nextCheck_; }
    // Before project.new/open, while callbacks still describe the old project.
    void prepareProjectSwitch();
    // Writes now if the state differs (exit, explicit save).
    Status flush(const std::filesystem::path& project);
    static std::filesystem::path fileFor(const std::filesystem::path& project);

  private:
    json::Value collect() const;
    void drain();  // waits until the writer is idle
    void writerLoop(std::stop_token stop);
    void apply(const json::Value& doc);
    struct Section {
        std::string name;
        Save save;
        Load load;
        json::Value defaults;
    };
    std::vector<Section> sections_;
    std::filesystem::path current_;
    std::uint64_t generation_ = 0;
    bool initialized_ = false, prepared_ = false, protectedFile_ = false;
    std::string written_;  // canonical text last read or written (or handed to the writer)
    std::optional<std::chrono::steady_clock::time_point> nextCheck_;
    const std::function<void()> completed_;  // only wakes/posts, never accesses UI on the worker
    // The writer: one pending (file, text), the newest; a failed write makes
    // the completion wake arm a bounded retry.
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<std::pair<std::filesystem::path, std::string>> pending_;
    bool writing_ = false;
    std::atomic<bool> writeFailed_{false};
    std::jthread writer_;  // last: joined first
};

}  // namespace supple::gui
