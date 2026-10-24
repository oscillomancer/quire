// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A lazily created utility window, with its own retained tree and GL resources.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include "supple/json/value.h"
#include "supple/ui/root.h"

namespace supple::gui {

class FloatingPanel {
  public:
    FloatingPanel(const Platform& platform, Window& owner, ui::Root& ownerRoot,
                  std::filesystem::path fonts, std::string title, int width, int height,
                  int minWidth, int minHeight);
    ~FloatingPanel();
    Status ensure();
    void open(std::unique_ptr<ui::Node> content, std::function<void()> closed,
              std::optional<ui::Rect> anchor = std::nullopt);
    void raise();
    void close();
    bool isOpen() const { return open_; }
    bool handle(const SDL_Event& event);  // consumes only this window's events
    void frame(bool present = true);
    void setTheme(const ui::Theme& theme);
    bool hasFocus() const;
    ui::Root& root() const { return *root_; }  // after ensure()
    Window* window() const { return window_.get(); }
    json::Value saveLayout() const;
    void loadLayout(const json::Value& layout);
    Status snapshot(const std::filesystem::path& path);
    std::function<bool(const ui::KeyEvent&, ui::Root&)> onUnhandledKey;

  private:
    void minimumSize();
    void place(std::optional<ui::Rect> anchor);
    const Platform& platform_;
    Window& owner_;
    ui::Root& ownerRoot_;
    std::filesystem::path fonts_;
    std::string title_;
    int width_, height_, minWidth_, minHeight_;
    bool open_ = false;
    json::Value placement_;
    std::function<void()> closed_;
    std::unique_ptr<Window> window_;
    std::unique_ptr<ui::Root> root_;
};

}  // namespace supple::gui
