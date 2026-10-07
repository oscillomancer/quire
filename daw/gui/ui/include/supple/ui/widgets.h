// Basic widgets. Each reads theme tokens only and exposes its value for
// tests (valueText).
#pragma once

#include <functional>
#include <string>

#include "supple/ui/node.h"

namespace supple::ui {

class Label : public Node {
  public:
    Label(std::string id, std::string text, Font font = Font::Ui, std::string sizeToken = "body",
          std::string colorToken = "ink");
    void setText(std::string text);
    const std::string& text() const { return text_; }
    int align = kLeft | kMiddle;
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    std::string valueText() const override { return text_; }

  private:
    std::string text_;
    Font font_;
    std::string size_, color_;
};

class Button : public Node {
  public:
    Button(std::string id, std::string text, std::function<void()> onClick = {});
    std::function<void()> onClick;
    void setText(std::string text);
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override { return text_; }

  private:
    std::string text_;
    bool pressed_ = false;
};

}  // namespace supple::ui
