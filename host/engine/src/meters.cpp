#include "meters.h"

#include <algorithm>

namespace supple::engine {

namespace {
// The bank's main-thread constructor state lives in the header; this keeps the
// slot table's free list in one place.
std::vector<std::uint16_t> allSlots() {
    std::vector<std::uint16_t> free;
    free.reserve(MeterBank::kSlots);
    for (std::size_t i = MeterBank::kSlots; i-- > 0;) free.push_back(static_cast<std::uint16_t>(i));
    return free;
}
}  // namespace

MeterBank::MeterBank() : freeSlots_(allSlots()), pending_(kSlots) {}

void MeterBank::beginPlan() {
    for (Slot& slot : slots_) slot.used = false;
}

std::uint16_t MeterBank::slotFor(model::Id track) {
    if (const auto it = byTrack_.find(track); it != byTrack_.end()) {
        slots_[it->second].used = true;
        return it->second;
    }
    if (freeSlots_.empty()) return kNoSlot;  // more strips than slots: unmetered
    const std::uint16_t slot = freeSlots_.back();
    freeSlots_.pop_back();
    slots_[slot] = {track, true};
    byTrack_[track] = slot;
    return slot;
}

void MeterBank::endPlan() {
    for (auto it = byTrack_.begin(); it != byTrack_.end();) {
        if (slots_[it->second].used) {
            ++it;
            continue;
        }
        // The plan no longer has this track: hand the slot back.
        const std::uint16_t slot = it->second;
        pending_[slot].clear();
        freeSlots_.push_back(slot);
        it = byTrack_.erase(it);
    }
}

void MeterBank::drain(Ring& ring) {
    MeterEntry entry;
    while (ring.pop(entry)) {
        if (entry.slot >= kSlots) continue;
        std::vector<MeterEntry>& list = pending_[entry.slot];
        if (list.size() >= kMaxPendingPerSlot)
            list.erase(list.begin());  // never heard: newest wins
        list.push_back(entry);
    }
}

std::vector<MeterReading> MeterBank::read(std::uint64_t epoch, std::int64_t heardStream) {
    drain(renderer_);
    drain(callback_);
    std::vector<MeterReading> out;
    out.reserve(byTrack_.size());
    for (std::size_t s = 0; s < kSlots; ++s) {
        if (!slots_[s].used) {
            pending_[s].clear();  // a track the newest plan dropped
            continue;
        }
        std::vector<MeterEntry>& list = pending_[s];
        MeterReading reading;
        reading.track = slots_[s].track;
        double sums[2] = {0.0, 0.0};
        std::int64_t frames = 0;
        std::size_t keep = 0;
        for (const MeterEntry& entry : list) {
            if (entry.epoch < epoch) continue;  // a seek or pause flushed it
            if (entry.epoch == epoch && entry.stream + entry.frames <= heardStream) {
                reading.peak[0] = std::max(reading.peak[0], entry.peak[0]);
                reading.peak[1] = std::max(reading.peak[1], entry.peak[1]);
                sums[0] += static_cast<double>(entry.sumSq[0]);
                sums[1] += static_cast<double>(entry.sumSq[1]);
                frames += entry.frames;
            } else {
                list[keep++] = entry;  // ahead of the listener: keep it
            }
        }
        list.resize(keep);
        if (frames > 0) {
            reading.rms[0] = static_cast<float>(std::sqrt(sums[0] / static_cast<double>(frames)));
            reading.rms[1] = static_cast<float>(std::sqrt(sums[1] / static_cast<double>(frames)));
        }
        out.push_back(reading);
    }
    return out;
}

std::size_t MeterBank::pendingEntries() const {
    std::size_t n = 0;
    for (const auto& list : pending_) n += list.size();
    return n;
}

}  // namespace supple::engine
