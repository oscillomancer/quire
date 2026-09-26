// Internal: the compiled render plan shared by the offline renderer and the
// real-time engine (engine.md §1–2). Not a public interface.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "supple/engine/offline.h"
#include "supple/time/timeline.h"

namespace supple::engine::detail {

using model::Id;
using model::Kind;
using model::Session;

inline double dbToGain(double db) {
    return db <= model::kSilenceDb ? 0.0 : std::pow(10.0, db / 20.0);
}

// A fixed-length delay for one stereo signal (allocated at compile time).
struct Delay {
    std::int64_t length = 0;
    std::vector<float> buf[2];
    std::size_t pos = 0;
    void init(std::int64_t n) {
        length = n;
        for (auto& b : buf) b.assign(static_cast<std::size_t>(std::max<std::int64_t>(n, 1)), 0.0f);
        pos = 0;
    }
    void clear() SUPPLE_RT {
        for (auto& b : buf) std::fill(b.begin(), b.end(), 0.0f);
        pos = 0;
    }
    // In place on `frames` samples of L/R.
    void process(float* l, float* r, std::uint32_t frames) SUPPLE_RT {
        if (length == 0) return;
        const auto n = static_cast<std::size_t>(length);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float dl = buf[0][pos], dr = buf[1][pos];
            buf[0][pos] = l[i];
            buf[1][pos] = r[i];
            l[i] = dl;
            r[i] = dr;
            pos = pos + 1 == n ? 0 : pos + 1;
        }
    }
};

struct Stereo {
    std::vector<float> l, r;
    void init(std::size_t n) {
        l.assign(n, 0.0f);
        r.assign(n, 0.0f);
    }
    void clear(std::uint32_t n) SUPPLE_RT {
        std::fill_n(l.begin(), n, 0.0f);
        std::fill_n(r.begin(), n, 0.0f);
    }
    void copyFrom(const Stereo& o, std::uint32_t n) SUPPLE_RT {
        std::copy_n(o.l.begin(), n, l.begin());
        std::copy_n(o.r.begin(), n, r.begin());
    }
};

inline double shapeCurve(model::PointShape shape, double tension, double t) {
    switch (shape) {
        case model::PointShape::Linear:
            return t;
        case model::PointShape::Square:
            return 0.0;  // holds until the next point
        case model::PointShape::Slow:
            return t * t * (3.0 - 2.0 * t);
        case model::PointShape::FastStart:
            return 1.0 - (1.0 - t) * (1.0 - t);
        case model::PointShape::FastEnd:
            return t * t;
        case model::PointShape::Bezier:
            return t + std::clamp(tension, -1.0, 1.0) * t * (1.0 - t);
    }
    return t;
}

// A compiled envelope: points at samples from the render start (already
// shifted by the latency at the point of application), values in the
// target's unit. Playback is monotonic, so a cursor makes lookups O(1).
struct Automation {
    struct Pt {
        std::int64_t sample;
        double value;
        model::PointShape shape;
        double tension;
    };
    std::vector<Pt> points;
    std::size_t cursor = 0;
    bool active() const { return !points.empty(); }
    void shift(std::int64_t by) {
        for (auto& pt : points) pt.sample += by;
    }
    double at(std::int64_t s) SUPPLE_RT {
        if (s <= points.front().sample) return points.front().value;
        while (cursor + 1 < points.size() && points[cursor + 1].sample <= s) ++cursor;
        if (cursor + 1 >= points.size()) return points.back().value;
        const Pt& a = points[cursor];
        const Pt& b = points[cursor + 1];
        const double t =
            static_cast<double>(s - a.sample) / static_cast<double>(b.sample - a.sample);
        return a.value + (b.value - a.value) * shapeCurve(a.shape, a.tension, t);
    }
};

struct Edge {
    std::size_t from = 0;     // node index
    std::optional<Id> route;  // empty for parent sends
    model::Tap tap = model::Tap::PostFader;
    double gainL = 1.0, gainR = 1.0;
    double volumeDb = 0.0, pan = 0.0;
    bool mono = false, invert = false;
    Automation autoVolume, autoPan, autoMute;  // route envelopes (dB, %, 0/1)
    Delay delay;
    Stereo tmp;
};

struct Stage {
    host::Instance* instance = nullptr;  // null: placeholder (pass-through)
    bool bypass = false, instrument = false;
    double wet = 1.0;
    std::int64_t latency = 0;
    Delay dry;  // bypassed or partly-dry signal, delayed to match the stage
    Stereo out, dryBuf;
    std::vector<host::Event> blockEvents;
    std::vector<host::OutputEvent> outStorage;
    host::OutputEvents outEvents;
};

// An audio item ready to play: which source frames land on which samples.
struct Clip {
    std::shared_ptr<const audio::AudioData> data;
    std::int64_t start = 0, length = 0;  // samples on the track's timeline (PDC-shifted)
    std::int64_t offset = 0;             // source frame at the item start
    bool loop = false;
    model::ChannelMode mode = model::ChannelMode::Normal;
    double gainL = 1.0, gainR = 1.0;
    std::int64_t fadeIn = 0, fadeOut = 0;
    model::FadeShape fadeInShape = model::FadeShape::Linear,
                     fadeOutShape = model::FadeShape::Linear;
};

struct TimedEvent {
    std::int64_t sample;
    host::Event event;
};

struct Node {
    Id id;
    std::vector<std::size_t> in;  // incoming edges, in summation order
    std::vector<Stage> chain;
    std::int64_t latIn = 0, latChain = 0;
    Stereo sum, preFx, preFader, post;
    std::vector<TimedEvent> events;
    std::size_t nextEvent = 0;
    std::vector<Clip> clips;
    int noteTarget = -1;  // chain index receiving note events
    double volume = 1.0, width = 1.0, panL = 1.0, panR = 1.0;
    double volumeDb = 0.0, pan = 0.0, panLawDb = -3.0;
    bool mute = false, master = false;
    bool soloSilenced = false;  // another track is soloed and this one is not on its path
    Automation autoVolume, autoTrim, autoPan, autoWidth, autoMute;  // dB, dB, %, %, 0/1
    bool automated() const {
        return autoVolume.active() || autoTrim.active() || autoPan.active() || autoWidth.active() ||
               autoMute.active();
    }
};

struct Plan {
    std::vector<Node> nodes;  // topological order; the master is last
    std::vector<Edge> edges;
    double rate = 48000.0;
    double startSeconds = 0.0;
    std::int64_t outputLatency = 0;
    std::uint32_t block = 512;
    // Shared with the session version it was compiled from: the plan must not
    // depend on that session staying alive (the real-time engine outlives it).
    std::shared_ptr<const time::TimeMap> timeMap;
    std::vector<std::string> warnings;
    std::filesystem::path projectDir;
    MediaCache* media = nullptr;
    std::vector<host::Instance*> instances;  // every instance the plan processes, sorted
};

// Builds the plan's structure (nodes in topological order, edges, plugin
// instances from the pool, automation). `startSeconds` is the timeline
// origin of sample 0 (0 for the real-time engine).
Result<Plan> compile(const Session& s, InstancePool& pool, double startSeconds,
                     std::uint32_t block);
// Main thread: activates new instances, measures latencies, sizes buffers
// and delays, compiles events and clips.
Status prepare(Plan& p, const Session& s);
// Audio thread: one block of every node at plan sample `pos`.
bool processBlock(Plan& p, std::int64_t pos, std::uint32_t frames) SUPPLE_RT;
// Audio thread: repositions every playback cursor at `pos` (events,
// automation) and clears delay lines. Plugins are not touched.
void locate(Plan& p, std::int64_t pos) SUPPLE_RT;
host::Transport transportAt(const Plan& p, std::int64_t sample);

}  // namespace supple::engine::detail
