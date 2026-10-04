// Row geometry shared by the arrange view's two columns (gui.md §4.3): one
// ordered row list per session version, one height per row and the same
// vertical scroll in the track control panel and the lanes. UI state per
// gui.md §1.2: the global row height and per-track overrides live here, never
// in the session.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "supple/model/session.h"
#include "supple/views/envelope_lane.h"

namespace supple::views {

// A visible row: a track and its indentation under the first root (the
// masters' children are at depth 0). An automation lane is a row too: it
// carries its track (and type/depth) plus the envelope it shows (P5.14).
struct TrackRow {
    model::Id track;
    model::StripType type = model::StripType::Channel;
    int depth = 0;
    std::optional<model::Id> env;  // set on an automation lane
};

// The arrange view's horizontal viewport and vertical scroll (gui.md §1.2:
// zoom and scroll are UI state).
struct Viewport {
    double startSeconds = 0;      // time at the left edge of the lanes
    double pixelsPerSecond = 40;  // design units per second
    float scrollY = 0;            // design units scrolled past the first row
};

class TrackLayout {
  public:
    static constexpr float kDefaultHeight = 72;
    static constexpr float kMinHeight = 8;
    static constexpr float kMaxHeight = 400;

    // Rebuilds the rows when the session's version moved; true then. A track
    // whose ancestor is a collapsed folder (Track::folderCompact == 2) is
    // hidden; a "small" folder (== 1) gets half the global height.
    bool show(const model::Session& session);

    const model::Session& session() const { return session_; }
    const std::vector<TrackRow>& rows() const { return rows_; }
    std::size_t rowCount() const { return rows_.size(); }
    std::optional<std::size_t> indexOf(model::Id track) const;   // the track's own row
    std::optional<std::size_t> indexOfEnv(model::Id env) const;  // an automation lane
    float envHeight() const;  // an automation lane's height (follows the global height)

    float globalHeight() const { return globalHeight_; }
    void setGlobalHeight(float h);  // vertical zoom
    // Per-track height in design units; <= 0 clears the override.
    void setTrackHeight(model::Id track, float h);
    void clearTrackHeights();  // every track back to the global height
    bool hasTrackHeight(model::Id track) const { return heights_.count(track) != 0; }
    // The explicit per-track overrides, by track (saving UI state).
    const std::map<model::Id, float>& heights() const { return heights_; }
    float heightOf(std::size_t row) const { return rowHeights_[row]; }
    float yOf(std::size_t row) const { return yOffsets_[row]; }  // content space
    float contentHeight() const { return contentHeight_; }
    // A plugin parameter envelope's scale (the app reads fx.describe); empty
    // or nullopt: plugin lanes are not shown.
    std::function<std::optional<EnvScale>(const model::Envelope&)> pluginScales;
    // A lane's scale: a mixer parameter's, else the plugin's.
    std::optional<EnvScale> scaleOf(const model::Envelope& env) const;
    // Called when the rows or a height changed (the view re-clamps its scroll,
    // refreshes its scroll bars and repaints).
    std::function<void()> onChanged;

  private:
    float baseHeight(model::Id track) const;
    void rebuildGeometry();

    model::Session session_;
    std::uint64_t indexed_ = ~std::uint64_t{0};
    std::vector<TrackRow> rows_;
    std::map<model::Id, float> heights_;
    float globalHeight_ = kDefaultHeight;
    std::vector<float> rowHeights_;
    std::vector<float> yOffsets_;
    float contentHeight_ = 0;
};

}  // namespace supple::views
