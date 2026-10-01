// Per-project UI state (docs/specs/gui.md §1.2): zoom, scroll, selection,
// panels and window size live in <project stem>.ui.json beside the project
// (canonical JSON, never in the project or its diff). Views register a
// section each; the file is read when the project path changes and written
// (at most once a second) when the serialized state differs from the file.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/value.h"

namespace supple::gui {

class UiPersistence {
  public:
    using Save = std::function<json::Value()>;
    using Load = std::function<void(const json::Value&)>;
    void add(std::string section, Save save, Load load);

    // Call every loop pass with the project's current path (empty: unsaved).
    void tick(const std::filesystem::path& project);
    // Writes now if the state differs (exit, explicit save).
    Status flush(const std::filesystem::path& project);
    static std::filesystem::path fileFor(const std::filesystem::path& project);

  private:
    json::Value collect() const;
    void apply(const json::Value& doc);
    struct Section {
        std::string name;
        Save save;
        Load load;
    };
    std::vector<Section> sections_;
    std::filesystem::path current_;
    std::string written_;  // canonical text last read or written
    std::chrono::steady_clock::time_point nextCheck_{};
};

}  // namespace supple::gui
