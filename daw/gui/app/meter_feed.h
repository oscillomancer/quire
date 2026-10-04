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
    void attach(const std::string& track, ui::Meter* meter) { meters_[track].push_back(meter); }
    void detach(ui::Meter* meter) {
        for (auto it = meters_.begin(); it != meters_.end();) {
            std::erase(it->second, meter);
            if (it->second.empty())
                it = meters_.erase(it);
            else
                ++it;
        }
    }
    void detachAll() { meters_.clear(); }
    // Every frame.
    void poll() {
        if (source_)
            for (const StripReading& r : source_())
                if (auto it = meters_.find(r.track); it != meters_.end())
                    for (ui::Meter* m : it->second) m->push(r.peak, r.rms);
        for (auto& [track, list] : meters_)
            for (ui::Meter* m : list)
                if (m->visible) m->tick();
    }

  private:
    Source source_;
    std::map<std::string, std::vector<ui::Meter*>> meters_;
};

}  // namespace supple::gui
