// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The routing matrix (docs/specs/gui.md §4.6): strips (in hierarchy) ×
// strips and plug-in keys, from route.matrix. A strip with auxiliary channels
// (a multi-out instrument's outputs, ADR-0021) has a row per channel pair
// under its own, named by the outputs. A cell creates a send (from that
// pair); a cell with a send selects it for editing (tap, level, pan, mute,
// remove); cells the routing rules reject are drawn disabled and say why on
// hover.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app_actions.h"
#include "floating_panel.h"
#include "supple/ui/root.h"

namespace supple::ui {
class Label;
class TextField;
class Button;
}  // namespace supple::ui

namespace supple::gui {

struct MatrixModel {
    struct Source {
        std::string id, name, type, parent;
        int depth = 0;
        bool parentSend = true;
        std::string rule;  // why it cannot feed strips (masters)
        // The channels the row sends (ADR-0021): 1 is the strip's main pair;
        // 3, 5... its auxiliary pairs (width 1: a last odd channel), whose
        // rows follow the strip's with the outputs' names.
        int pair = 1, width = 2;
        std::string strip;  // the strip's name, for an auxiliary row
    };
    struct Destination {
        std::string id, name, kind, type, track;  // kind: track | key
        std::string rule;                         // why no strip can feed it (channels)
    };
    struct Send {
        std::string id, from, to, tap;
        double volumeDb = 0, pan = 0;
        bool mute = false, mono = false, phaseInvert = false;
        std::vector<int> mapFrom, mapTo;  // the channel map (empty: identity)
    };
    std::vector<Source> sources;
    std::vector<Destination> destinations;
    std::vector<Send> sends;
    std::vector<std::pair<std::string, std::string>> cyclePairs;  // (from, to)
    std::vector<std::string> cyclePaths;
    void index();
    std::map<std::pair<std::string, std::string>, std::string> cycleIndex;
    std::map<std::pair<std::string, std::string>, std::vector<std::size_t>> sendIndex;
    // Why from -> to is refused (empty: allowed).
    std::string blocked(std::size_t row, std::size_t col) const;
    std::vector<const Send*> sendsAt(std::size_t row, std::size_t col) const;
    // The source pair a send leaves from: 1 (the main pair), 3, 5...
    static int sourcePair(const Send& x);
    static std::string pairText(int pair, int width);  // "3/4", "5"
};

class RoutingMatrix {
  public:
    RoutingMatrix(FloatingPanel& panel, AppActions& actions, cmd::Dispatcher& dispatcher,
                  engine::EngineApp& app);
    ~RoutingMatrix();
    void close() { panel_.close(); }
    void open(std::optional<ui::Rect> anchor = std::nullopt);
    bool isOpen() const { return grid_ != nullptr; }
    void sync();  // reload when the session changed while open

    // Tests.
    const MatrixModel& model() const { return model_; }
    ui::Node* grid() const { return grid_; }
    Status clickCell(std::size_t row, std::size_t col);
    std::optional<std::string> selectedSend() const { return selected_; }
    static std::string channelsText(const MatrixModel::Send& x);
    std::string status() const;

  private:
    Status reload();
    void select(std::optional<std::string> send);
    Result<json::Value> exec(const std::string& command, json::Value args);
    void setSend(const char* field, json::Value value);
    ui::Root& root() const { return panel_.root(); }
    FloatingPanel& panel_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    MatrixModel model_;
    std::uint64_t version_ = 0;
    std::uint64_t projectGeneration_ = ~std::uint64_t{0};
    std::optional<std::string> selected_;
    ui::Node* grid_ = nullptr;
    ui::Label* status_ = nullptr;
    ui::Label* sendTitle_ = nullptr;
    ui::Node* details_ = nullptr;
    ui::TextField* volume_ = nullptr;
    ui::TextField* pan_ = nullptr;
    ui::Button* mute_ = nullptr;
    ui::Button* channels_ = nullptr;  // the channel map (a menu)
    ui::Button* phase_ = nullptr;
    std::vector<ui::Button*> taps_;
};

}  // namespace supple::gui
