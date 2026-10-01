// The track lanes: every non-master strip as a row of items, drawn with the
// bulk GL layers (docs/specs/gui.md §3): item bodies and MIDI notes as
// instanced rectangles, audio as min/max bands sampled from peak pyramids.
// Rows and items outside the painted region are skipped (virtualized).
// P5.4 builds the arrange view (ruler, playhead, selection) on top of this.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

#include "supple/model/session.h"
#include "supple/peaks/peaks.h"
#include "supple/ui/gl.h"
#include "supple/ui/node.h"

namespace supple::views {

struct Viewport {
    double startSeconds = 0;      // time at the left edge
    double pixelsPerSecond = 40;  // design units per second
    float scrollY = 0;            // design units scrolled past the first row
    float rowHeight = 72;
};

class Lanes : public ui::Node {
  public:
    using Resolve = std::function<std::filesystem::path(const model::MediaSource&)>;
    Lanes(peaks::PeakService& peaks, Resolve resolve);

    // Takes the session to show (cheap: sessions share structure); rebuilds
    // the row index when the version moved.
    void show(const model::Session& session);
    const Viewport& viewport() const { return view_; }
    void setViewport(const Viewport& v);
    float contentHeight() const { return static_cast<float>(rows_.size()) * view_.rowHeight; }

    void arrange(const ui::Canvas& canvas, const ui::Rect& bounds) override;
    void paint(ui::Canvas& canvas) override;
    void paintGl(const ui::GlFrame& frame) override;
    bool onPointer(const ui::PointerEvent& e) override;

    // What the last paint drew (benchmarks, tests).
    struct Stats {
        std::size_t rows = 0, items = 0, columns = 0, notes = 0, pending = 0;
    };
    const Stats& stats() const { return stats_; }

  private:
    struct Row {
        model::Id track;
        model::StripType type;
        std::vector<model::Id> items;  // by position
    };
    friend class LaneLabels;
    double timeAt(float x) const {
        return view_.startSeconds + static_cast<double>(x - bounds().x) / view_.pixelsPerSecond;
    }
    float xAt(double seconds) const {
        return bounds().x +
               static_cast<float>((seconds - view_.startSeconds) * view_.pixelsPerSecond);
    }
    ui::Rect rowRect(std::size_t i) const;
    ui::Rect itemRect(std::size_t row, const model::Item& item) const;
    void drawAudio(const ui::GlFrame& frame, const ui::Rect& r, const model::Item& item,
                   const model::Take& take, const model::MediaSource& source, std::uint32_t rgba);
    void drawMidi(const ui::Rect& r, const model::Item& item, const model::Take& take,
                  const model::MidiSource& source, std::uint32_t rgba, std::uint32_t border);

    peaks::PeakService& peaks_;
    Resolve resolve_;
    model::Session session_;
    std::uint64_t indexed_ = ~std::uint64_t{0};
    std::vector<Row> rows_;
    Viewport view_;
    std::optional<ui::RectRenderer> bodies_, notes_;
    std::optional<ui::StripRenderer> strips_;
    bool glFailed_ = false;
    const ui::Theme* theme_ = nullptr;  // from the last paint (paintGl has no canvas)
    Stats stats_;
};

}  // namespace supple::views
