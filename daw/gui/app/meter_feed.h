// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Meter readings to meter widgets: once per frame the feed polls its source
// (EngineApp::meters() in the app — the audio heard since the last poll) and
// hands each strip's reading to every Meter registered for that track (the
// TCP's and the mixer's), then lets every visible meter decay.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "supple/ui/controls.h"

namespace supple::gui {

struct StripReading {
    std::string track;  // "tr_3"
    float peak[2] = {0, 0}, rms[2] = {0, 0};
};

class MeterFeed {
  public:
    using Source = std::function<std::vector<StripReading>()>;
    explicit MeterFeed(Source source = {}) : source_(std::move(source)) {}
    void setSource(Source source) { source_ = std::move(source); }
    // A meter shows `track`; forget it before the node goes away.
    void attach(const std::string& track, ui::Meter* meter) {
        meters_[track].push_back(meter);
        shown_[meter] = meter->visibleInTree();
    }
    void detach(ui::Meter* meter) {
        shown_.erase(meter);
        for (auto it = meters_.begin(); it != meters_.end();) {
            std::erase(it->second, meter);
            if (it->second.empty())
                it = meters_.erase(it);
            else
                ++it;
        }
    }
    void detachAll() {
        meters_.clear();
        shown_.clear();
    }
    // Every frame.
    void poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        for (auto& [meter, shown] : shown_) {
            const bool visible = meter->visibleInTree();
            if (visible && !shown) meter->reset();
            shown = visible;
        }
        if (source_)
            for (const StripReading& r : source_())
                if (auto it = meters_.find(r.track); it != meters_.end())
                    for (ui::Meter* m : it->second)
                        if (shown_[m]) m->push(r.peak, r.rms, now);
        for (auto& [track, list] : meters_)
            for (ui::Meter* m : list)
                if (shown_[m]) m->tick(now);
    }
    bool moving() const {
        for (const auto& [meter, shown] : shown_)
            if (shown && meter->moving()) return true;
        return false;
    }
    bool visible() const {
        for (const auto& [meter, shown] : shown_)
            if (shown) return true;
        return false;
    }

  private:
    std::map<ui::Meter*, bool> shown_;
    Source source_;
    std::map<std::string, std::vector<ui::Meter*>> meters_;
};

}  // namespace supple::gui
