// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Gui.h"

#include <algorithm>
#include <cmath>

namespace supple::gui {

Gui::Gui(EditorHost& host, const char* className, bool standalone)
    : editor_(product::createEditor(host)), window_(*editor_, className, standalone) {}

Gui::~Gui() {
    editor_->focusLost();  // closes any open gesture before the host loses us
    if (retired_) retired_->focusLost();
    window_.destroy();
}

void Gui::rebuild(EditorHost& host) {
    std::unique_ptr<EditorBase> next = product::createEditor(host);
    editor_->focusLost();
    window_.replaceEditor(*next);
    // The editor the window shows lives until the swap; one that was only
    // waiting for it goes now.
    if (!retired_) retired_ = std::move(editor_);
    editor_ = std::move(next);
    if (!window_.replacing()) retired_.reset();
    applyLimits();
}

bool Gui::create() {
    if (!window_.create()) return false;
    applyLimits();
    window_.setSize(hostW(userScale_), hostH(userScale_));
    return true;
}

uint32_t Gui::hostW(double s) const {
    return static_cast<uint32_t>(std::lround(editor_->width() * s * platformScale_));
}
uint32_t Gui::hostH(double s) const {
    return static_cast<uint32_t>(std::lround(editor_->height() * s * platformScale_));
}

void Gui::applyLimits() {
    window_.setSizeLimits(hostW(kMinScale), hostH(kMinScale), hostW(kMaxScale), hostH(kMaxScale));
}

void Gui::size(uint32_t& w, uint32_t& h) const {
    w = window_.width();
    h = window_.height();
}

bool Gui::adjustSize(uint32_t& w, uint32_t& h) const {
    const double sx = static_cast<double>(w) / (editor_->width() * platformScale_);
    const double sy = static_cast<double>(h) / (editor_->height() * platformScale_);
    // Fitting inside matters: a host given back more than it offered clips.
    const double s = std::clamp(std::min(sx, sy), kMinScale, kMaxScale);
    w = hostW(s);
    h = hostH(s);
    return true;
}

bool Gui::setSize(uint32_t w, uint32_t h) {
    // A host that ignores the aspect still gets an undistorted panel: Window
    // letterboxes. The user scale follows whichever axis is limiting.
    const double sx = static_cast<double>(w) / (editor_->width() * platformScale_);
    const double sy = static_cast<double>(h) / (editor_->height() * platformScale_);
    if (!window_.setSize(w, h)) return false;
    userScale_ = std::clamp(std::min(sx, sy), kMinScale, kMaxScale);
    return true;
}

void Gui::setPlatformScale(double scale) {
    if (!std::isfinite(scale) || scale <= 0.0) return;
    // Scale the current size by the ratio rather than recomputing it from the
    // user scale, so 2x and back again lands on exactly the same pixels.
    const double next = std::clamp(scale, 0.5, 4.0);
    const double ratio = next / platformScale_;
    const uint32_t w = static_cast<uint32_t>(std::lround(window_.width() * ratio));
    const uint32_t h = static_cast<uint32_t>(std::lround(window_.height() * ratio));
    platformScale_ = next;
    applyLimits();
    window_.setSize(w, h);
}

void Gui::sizeAt(double scale, uint32_t& w, uint32_t& h) const {
    const double s = std::isfinite(scale) && scale > 0.0 ? std::clamp(scale, kMinScale, kMaxScale) : userScale_;
    w = hostW(s);
    h = hostH(s);
}

void Gui::setUserScale(double scale) {
    if (!std::isfinite(scale) || scale <= 0.0) return;
    userScale_ = std::clamp(scale, kMinScale, kMaxScale);
    window_.setSize(hostW(userScale_), hostH(userScale_));
}

void Gui::aspect(uint32_t& w, uint32_t& h) const {
    w = static_cast<uint32_t>(std::lround(editor_->width()));
    h = static_cast<uint32_t>(std::lround(editor_->height()));
}

}  // namespace supple::gui
