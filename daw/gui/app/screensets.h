// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Named screensets (gap log #38): the window's layout — which panels show and
// the arrange view's zoom, scroll, row heights (global and per track) and
// follow mode — saved under a name and recalled in one step. They live in the
// project's UI file (<stem>.ui.json, gui.md §1.2), never in the project.
// Slots 1-5 are names too: Ctrl+Alt+N saves one, Alt+N recalls it.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/value.h"

namespace supple::gui {

class Screensets {
  public:
    using Capture = std::function<json::Value()>;  // the current layout
    using Apply = std::function<void(const json::Value&)>;
    Screensets(Capture capture, Apply apply)
        : capture_(std::move(capture)), apply_(std::move(apply)) {}

    Status save(const std::string& name);  // replaces a set of that name
    Status load(const std::string& name) const;
    Status remove(const std::string& name);
    std::vector<std::string> names() const;  // sorted
    bool has(const std::string& name) const { return sets_.count(name) != 0; }

    // The UI file's "screensets" section: {name: layout}.
    json::Value toJson() const;
    void fromJson(const json::Value& v);

  private:
    Capture capture_;
    Apply apply_;
    std::map<std::string, json::Value> sets_;
};

}  // namespace supple::gui
