// Per-strip peak/RMS telemetry for the GUI meters (engine.md §2.3, ADR-0014).
//
// The anticipative renderer processes audio up to `render_ahead` *before* it is
// heard, so a meter fed straight from the renderer would lead the sound. Two
// SPSC rings carry the numbers instead: the renderer thread writes what it
// renders ahead, the audio callback writes what it plays (the live nodes, or
// the whole graph when anticipation is off). A reader on the main thread
// reports an entry only once the callback's play position has passed the audio
// it covers — a meter shows what has been heard.
//
// Slots: a strip's slot is assigned once per track on the main thread, when
// the engine compiles a plan, and stays with that track for the bank's life so
// a reader can accumulate across plan swaps. The audio threads only read the
// slot number the plan carries (kNoSlot: unmetered); the slot table and the
// pending queues belong to the main thread.
//
// The producers never block and never allocate: a full ring drops the entry
// and counts it (droppedRenderer()/droppedCallback()).
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "supple/base/rt.h"
#include "supple/base/spsc.h"
#include "supple/engine/app.h"  // MeterReading (the public reading type)
#include "supple/model/ids.h"

namespace supple::engine {

// One block's measurements of one strip, in the same coordinates a Chunk uses
// (ADR-0012): the transport epoch and the frames processed in it before this
// block.
struct MeterEntry {
    std::uint64_t epoch = 0;
    std::int64_t stream = 0;
    std::uint32_t frames = 0;
    std::uint16_t slot = 0xffff;
    float peak[2] = {0.0f, 0.0f};   // |sample|, per channel
    float sumSq[2] = {0.0f, 0.0f};  // Σ sample², per channel
};

class MeterBank {
  public:
    MeterBank();

    static constexpr std::uint16_t kNoSlot = 0xffff;
    static constexpr std::size_t kSlots = 1024;
    static constexpr std::size_t kRingCapacity = 16384;  // entries, per ring
    // A strip whose entries can never be heard (the transport stopped on them)
    // keeps at most this many, newest; the main thread's memory stays bounded.
    static constexpr std::size_t kMaxPendingPerSlot = 256;

    // --- producers (one thread each: the renderer, the audio callback) -----------
    bool pushRenderer(const MeterEntry& entry) SUPPLE_RT {
        return push(renderer_, droppedRenderer_, entry);
    }
    bool pushCallback(const MeterEntry& entry) SUPPLE_RT {
        return push(callback_, droppedCallback_, entry);
    }
    // Entries a full ring dropped (main thread).
    std::uint64_t droppedRenderer() const {
        return droppedRenderer_.load(std::memory_order_relaxed);
    }
    std::uint64_t droppedCallback() const {
        return droppedCallback_.load(std::memory_order_relaxed);
    }

    // --- main thread: one plan's strips ------------------------------------------
    // A stable slot for `track`, assigned on first use; kNoSlot when all
    // kSlots are taken. beginPlan()/endPlan() bracket one plan's assignment:
    // slots of tracks the plan no longer has are released and reused.
    void beginPlan();
    std::uint16_t slotFor(model::Id track);
    void endPlan();

    // --- main thread: reading -----------------------------------------------------
    // Drains both rings, drops what can never be heard (an older epoch: a seek
    // or pause flushed it), and returns one reading per strip of the newest
    // plan: the max peak and the RMS over the entries whose audio has been
    // heard up to `heardStream` in `epoch` since the last call. A strip with
    // no heard audio reports zeros (the GUI does decay and peak hold).
    std::vector<MeterReading> read(std::uint64_t epoch, std::int64_t heardStream);

    // Tests and diagnostics.
    std::size_t pendingEntries() const;

  private:
    static bool push(SpscRing<MeterEntry, kRingCapacity>& ring, std::atomic<std::uint64_t>& dropped,
                     const MeterEntry& entry) SUPPLE_RT {
        if (ring.push(entry)) return true;
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    void drain(SpscRing<MeterEntry, kRingCapacity>& ring);

    using Ring = SpscRing<MeterEntry, kRingCapacity>;
    Ring renderer_, callback_;
    std::atomic<std::uint64_t> droppedRenderer_{0}, droppedCallback_{0};
    struct Slot {
        model::Id track;
        bool used = false;  // in the plan being compiled (beginPlan/endPlan)
    };
    std::array<Slot, kSlots> slots_{};
    std::map<model::Id, std::uint16_t> byTrack_;  // assigned tracks -> slot
    std::vector<std::uint16_t> freeSlots_;        // never-assigned + reusable
    // Released slots wait two plans before they are handed out again: the plan
    // that dropped a track is still playing while the next one is compiled, and
    // the one before it may still push an entry for the slot during a crossfade.
    std::vector<std::uint16_t> retiredPrev_, retired_;
    std::vector<std::vector<MeterEntry>> pending_;  // per slot
};

}  // namespace supple::engine
