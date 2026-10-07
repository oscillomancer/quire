// Per-project UI state (docs/specs/gui.md §1.2): zoom, scroll, selection,
// panels and window size live in <project stem>.ui.json beside the project
// (canonical JSON, never in the project or its diff). Views register a
// section each; the file is read on project replacement and written
// (at most once a second) when the serialized state differs from the file.
// Those periodic writes run on a writer thread, so a slow disk never stalls
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
    UiPersistence();
    ~UiPersistence();  // finishes a pending write
    UiPersistence(const UiPersistence&) = delete;
    UiPersistence& operator=(const UiPersistence&) = delete;
    using Save = std::function<json::Value()>;
    using Load = std::function<void(const json::Value&)>;
    void add(std::string section, Save save, Load load);

    // Call every loop pass with the project's current path (empty: unsaved).
    void tick(const std::filesystem::path& project);
    void tick(const std::filesystem::path& project, std::uint64_t generation);
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
    std::chrono::steady_clock::time_point nextCheck_{};
    // The writer: one pending (file, text), the newest; a failed write makes
    // the next tick write again.
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<std::pair<std::filesystem::path, std::string>> pending_;
    bool writing_ = false;
    std::atomic<bool> writeFailed_{false};
    std::jthread writer_;  // last: joined first
};

}  // namespace supple::gui
