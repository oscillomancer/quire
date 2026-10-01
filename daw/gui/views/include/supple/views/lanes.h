// The track lanes: the arrange view's item area (gui.md §4.3). Items are
// drawn with the bulk GL layers (§3): bodies, fades and MIDI notes as
// instanced rectangles, audio as min/max bands sampled from peak pyramids,
// fades as polyline bands. Rows and items outside the painted region are
// skipped (virtualized); the row geometry comes from the shared TrackLayout.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

#include "supple/model/session.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/gl.h"
#include "supple/ui/node.h"
#include "supple/views/track_layout.h"

namespace supple::views {

class Lanes : public ui::Node {
  public:
    using Resolve = std::function<std::filesystem::path(const model::MediaSource&)>;
    Lanes(peaks::PeakService& peaks, Resolve resolve, const TrackLayout& rowLayout,
          const Viewport& view);

    // Rebuilds the per-row item index from the layout's session, then
    // invalidates (the controller calls this after TrackLayout::show).
    void rebuild();

    // Time <-> x inside the lanes (the arrange controller and the ruler share
    // this mapping, so the ruler lines up with the items).
    double timeAt(float x) const {
        return view_.startSeconds + static_cast<double>(x - bounds().x) / view_.pixelsPerSecond;
    }
    float xAt(double seconds) const {
        return bounds().x +
               static_cast<float>((seconds - view_.startSeconds) * view_.pixelsPerSecond);
    }
    float contentHeight() const { return layout_.contentHeight(); }
    std::size_t rowCount() const { return layout_.rowCount(); }
    ui::Rect rowRect(std::size_t row) const;
    ui::Rect itemRect(std::size_t row, const model::Item& item) const;

    void arrange(const ui::Canvas& canvas, const ui::Rect& bounds) override;
    void paint(ui::Canvas& canvas) override;
    void paintGl(const ui::GlFrame& frame) override;

    // What the last paint drew (benchmarks, tests).
    struct Stats {
        std::size_t rows = 0, items = 0, columns = 0, notes = 0, pending = 0, fades = 0;
    };
    const Stats& stats() const { return stats_; }

  private:
    friend class LaneLabels;
    void drawAudio(const ui::GlFrame& frame, const ui::Rect& r, const model::Item& item,
                   const model::Take& take, const model::MediaSource& source, std::uint32_t rgba);
    void drawMidi(const ui::Rect& r, const model::Item& item, const model::Take& take,
                  const model::MidiSource& source, std::uint32_t rgba, std::uint32_t border);
    void drawFades(const ui::Rect& r, const model::Item& item, std::uint32_t rgba);

    peaks::PeakService& peaks_;
    Resolve resolve_;
    const TrackLayout& layout_;
    const Viewport& view_;
    std::vector<std::vector<model::Id>> rowItems_;  // by position within a row
    std::optional<ui::RectRenderer> bodies_, notes_;
    std::optional<ui::StripRenderer> strips_;
    bool glFailed_ = false;
    const ui::Theme* theme_ = nullptr;  // from the last paint (paintGl has no canvas)
    Stats stats_;
};

}  // namespace supple::views
