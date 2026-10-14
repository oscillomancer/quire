// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The agent panel (docs/specs/gui.md §5): a chat with an LLM that edits the
// session on its own branch; the human reads the diff, A/Bs it and accepts
// or rejects. Docked at the right of the main window.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "supple/agent/config.h"
#include "supple/agent/session.h"
#include "supple/engine/app.h"
#include "supple/ui/root.h"

namespace supple::ui {
class Button;
class Label;
class ScrollView;
class TextBlock;
class TextField;
}  // namespace supple::ui

namespace supple::gui {

class AgentPanel {
  public:
    using Post = std::function<void(std::function<void()>)>;
    AgentPanel(ui::Root& root, engine::EngineApp& app, const cmd::Registry& registry,
               cmd::Dispatcher& dispatcher, Post post, std::filesystem::path configFile,
               std::filesystem::path skillDir);
    ~AgentPanel();

    std::unique_ptr<ui::Node> build();  // the docked panel (hidden until toggled)
    void toggle();
    void setVisible(bool visible);
    bool visible() const;
    void openSettings();
    // For tests and scripts: what the input field would send.
    void send(const std::string& prompt);
    agent::Session* session() { return session_.get(); }
    agent::Config& config() { return config_; }

  private:
    void ensureSession();
    void append(std::unique_ptr<ui::Node> node);
    void refreshReview();
    std::string describeChange(const json::Value& change) const;
    void setBusy(bool busy);

    ui::Root& root_;
    engine::EngineApp& app_;
    const cmd::Registry& registry_;
    cmd::Dispatcher& dispatcher_;
    Post post_;
    std::filesystem::path configFile_, skillDir_;
    agent::Config config_;
    std::unique_ptr<agent::Session> session_;

    ui::Node* panel_ = nullptr;
    ui::Label* model_ = nullptr;
    ui::ScrollView* transcript_ = nullptr;
    ui::Node* review_ = nullptr;
    ui::Label* reviewTitle_ = nullptr;
    ui::TextBlock* reviewText_ = nullptr;
    ui::TextField* input_ = nullptr;
    ui::Button* send_ = nullptr;
    ui::TextBlock* speaking_ = nullptr;  // the assistant text block being streamed into
    std::map<std::string, ui::TextBlock*> tools_;
    int serial_ = 0;
};

}  // namespace supple::gui
