#include "supple/views/track_layout.h"

#include <algorithm>
#include <functional>
#include <iterator>

namespace supple::views {
namespace {

float clampHeight(float h) {
    return std::clamp(h, TrackLayout::kMinHeight, TrackLayout::kMaxHeight);
}

}  // namespace

bool TrackLayout::show(const model::Session& session) {
    if (session.version == indexed_) return false;
    session_ = session;
    indexed_ = session.version;
    rows_.clear();
    std::function<void(model::Id, int)> visit = [&](model::Id id, int depth) {
        const model::Track* t = session_.tracks.find(id);
        if (!t) return;
        const bool row = t->type != model::StripType::Master && !t->hiddenTcp;
        if (row) rows_.push_back({id, t->type, depth});
        if (t->folderCompact == 2) return;  // a collapsed folder hides its children
        for (model::Id c : t->children) visit(c, row ? depth + 1 : depth);
    };
    for (model::Id master : session_.project->masters) visit(master, 0);
    // Drop overrides for tracks that are gone (they would never be read again).
    for (auto it = heights_.begin(); it != heights_.end();)
        it = session_.tracks.contains(it->first) ? std::next(it) : heights_.erase(it);
    rebuildGeometry();
    return true;
}

std::optional<std::size_t> TrackLayout::indexOf(model::Id track) const {
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].track == track) return i;
    return std::nullopt;
}

void TrackLayout::setGlobalHeight(float h) {
    const float clamped = clampHeight(h);
    if (clamped == globalHeight_) return;
    globalHeight_ = clamped;
    rebuildGeometry();
}

void TrackLayout::setTrackHeight(model::Id track, float h) {
    if (h <= 0)
        heights_.erase(track);
    else
        heights_[track] = clampHeight(h);
    rebuildGeometry();
}

float TrackLayout::baseHeight(model::Id track) const {
    if (auto it = heights_.find(track); it != heights_.end()) return it->second;
    const model::Track* t = session_.tracks.find(track);
    if (t && t->folderCompact == 1) return clampHeight(globalHeight_ * 0.5f);
    return globalHeight_;
}

void TrackLayout::rebuildGeometry() {
    rowHeights_.clear();
    yOffsets_.clear();
    rowHeights_.reserve(rows_.size());
    yOffsets_.reserve(rows_.size());
    float y = 0;
    for (const TrackRow& r : rows_) {
        const float h = baseHeight(r.track);
        yOffsets_.push_back(y);
        rowHeights_.push_back(h);
        y += h;
    }
    contentHeight_ = y;
}

}  // namespace supple::views
