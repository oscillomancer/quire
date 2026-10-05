#include "supple/views/track_layout.h"

#include <algorithm>
#include <functional>
#include <iterator>

#include "supple/views/envelope_lane.h"
#include "supple/views/session_changes.h"

namespace supple::views {
namespace {

float clampHeight(float h) {
    return std::clamp(h, TrackLayout::kMinHeight, TrackLayout::kMaxHeight);
}

}  // namespace

std::vector<model::Id> displayChildren(const model::Session& session, const model::Track& parent) {
    std::vector<model::Id> out;
    out.reserve(parent.children.size());
    for (model::StripType type :
         {model::StripType::Channel, model::StripType::Buss, model::StripType::Master})
        for (model::Id id : parent.children)
            if (const model::Track* t = session.tracks.find(id); t && t->type == type)
                out.push_back(id);
    return out;
}

std::optional<std::size_t> reorderIndex(const model::Session& session, model::Id track,
                                        std::optional<model::Id> before) {
    const model::Track* t = session.tracks.find(track);
    if (!t || t->type == model::StripType::Master || !t->parent) return std::nullopt;
    const model::Track* parent = session.tracks.find(*t->parent);
    if (!parent || before == track) return std::nullopt;
    std::vector<model::Id> rest = parent->children;
    std::erase(rest, track);
    std::optional<std::size_t> at;
    if (before) {
        const model::Track* b = session.tracks.find(*before);
        if (!b || b->type != t->type || b->parent != t->parent) return std::nullopt;
        if (const auto found = std::find(rest.begin(), rest.end(), *before); found != rest.end())
            at = static_cast<std::size_t>(found - rest.begin());
    } else {  // after the last sibling of this type
        for (std::size_t i = 0; i < rest.size(); ++i)
            if (const model::Track* s = session.tracks.find(rest[i]); s && s->type == t->type)
                at = i + 1;
    }
    if (!at) return std::nullopt;
    // The siblings of this type, before and after: the same order is no move.
    auto sameType = [&](const std::vector<model::Id>& ids) {
        std::vector<model::Id> out;
        for (model::Id id : ids)
            if (const model::Track* s = session.tracks.find(id); s && s->type == t->type)
                out.push_back(id);
        return out;
    };
    std::vector<model::Id> after = rest;
    after.insert(after.begin() + static_cast<std::ptrdiff_t>(*at), track);
    if (sameType(after) == sameType(parent->children)) return std::nullopt;
    return at;
}

bool TrackLayout::show(const model::Session& session, const std::vector<model::Change>* changes) {
    if (session.version == indexed_) return false;
    bool geometry =
        indexed_ == ~std::uint64_t{0} || session.project->masters != session_.project->masters;
    for (auto id : changedObjects(session_.tracks, session.tracks, changes)) {
        const auto* before = session_.tracks.find(id);
        const auto* after = session.tracks.find(id);
        geometry |= !before || !after || before->type != after->type ||
                    before->parent != after->parent || before->children != after->children ||
                    before->hiddenTcp != after->hiddenTcp ||
                    before->folderCompact != after->folderCompact ||
                    before->chain != after->chain || before->inputChain != after->inputChain;
    }
    for (auto id : changedObjects(session_.envelopes, session.envelopes, changes)) {
        const auto* before = session_.envelopes.find(id);
        const auto* after = session.envelopes.find(id);
        auto lane = [](const model::Envelope* env) {
            return env && env->visible &&
                   (env->target.object.kind == model::Kind::Track ||
                    env->target.object.kind == model::Kind::Fx);
        };
        geometry |= (lane(after) && scaleOf(*after).has_value()) != scaledEnvs_.contains(id);
        geometry |=
            lane(before) != lane(after) || (lane(before) && lane(after) &&
                                            (before->target.object != after->target.object ||
                                             before->target.param != after->target.param));
    }
    // Changing the plug-in can change whether its parameter lanes have scales.
    for (auto id : changedObjects(session_.fx, session.fx, changes)) {
        const auto* before = session_.fx.find(id);
        const auto* after = session.fx.find(id);
        geometry |= !before || !after || before->plugin.id != after->plugin.id ||
                    before->plugin.format != after->plugin.format ||
                    before->plugin.version != after->plugin.version;
    }
    session_ = session;
    indexed_ = session.version;
    if (!geometry) return false;
    ++rebuilds_;
    rows_.clear();
    trackRows_.clear();
    envRows_.clear();
    scaledEnvs_.clear();
    std::map<model::Id, model::Id> fxOwners;
    for (const auto& track : session_.tracks) {
        for (auto fx : track.value->chain) fxOwners[fx] = track.id;
        for (auto fx : track.value->inputChain) fxOwners[fx] = track.id;
    }
    std::map<model::Id, std::vector<std::pair<int, model::Id>>> lanes;
    for (const auto& envelope : session_.envelopes) {
        if (!envelope.value->visible || !scaleOf(*envelope.value)) continue;
        if (envelope.value->target.object.kind == model::Kind::Track ||
            envelope.value->target.object.kind == model::Kind::Fx)
            scaledEnvs_.insert(envelope.id);
        auto owner = envelope.value->target.object;
        if (owner.kind == model::Kind::Fx) {
            const auto found = fxOwners.find(owner);
            if (found == fxOwners.end()) continue;
            owner = found->second;
        }
        if (owner.kind == model::Kind::Track)
            lanes[owner].emplace_back(envOrder(*envelope.value), envelope.id);
    }
    for (auto& [track, entries] : lanes) std::sort(entries.begin(), entries.end());
    std::function<void(model::Id, int)> visit = [&](model::Id id, int depth) {
        const model::Track* t = session_.tracks.find(id);
        if (!t) return;
        const bool row = t->type != model::StripType::Master && !t->hiddenTcp;
        if (row) {
            trackRows_[id] = rows_.size();
            rows_.push_back({id, t->type, depth, std::nullopt});
            if (const auto found = lanes.find(id); found != lanes.end())
                for (const auto& lane : found->second) {
                    envRows_[lane.second] = rows_.size();
                    rows_.push_back({id, t->type, depth, lane.second});
                }
        }
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
    const auto found = trackRows_.find(track);
    return found == trackRows_.end() ? std::nullopt : std::optional(found->second);
}

std::optional<std::size_t> TrackLayout::indexOfEnv(model::Id env) const {
    const auto found = envRows_.find(env);
    return found == envRows_.end() ? std::nullopt : std::optional(found->second);
}

std::optional<EnvScale> TrackLayout::scaleOf(const model::Envelope& env) const {
    if (auto mixer = envScale(env)) return mixer;
    if (env.target.object.kind == model::Kind::Fx && pluginScales) return pluginScales(env);
    return std::nullopt;
}

float TrackLayout::envHeight() const { return clampHeight(std::max(28.0f, globalHeight_ * 0.6f)); }

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

void TrackLayout::clearTrackHeights() {
    heights_.clear();
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
        const float h = r.env ? envHeight() : baseHeight(r.track);
        yOffsets_.push_back(y);
        rowHeights_.push_back(h);
        y += h;
    }
    contentHeight_ = y;
    if (onChanged) onChanged();
}

}  // namespace supple::views
