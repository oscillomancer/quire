#include "recording.h"

#include <algorithm>
#include <chrono>

#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/json/json.h"

namespace supple::engine::detail {

std::filesystem::path sidecarPath(const std::filesystem::path& wav) {
    std::filesystem::path p = wav;
    p.replace_extension(".rec.json");
    return p;
}

Status writeSidecar(const std::filesystem::path& wav, const RecordingInfo& info) {
    json::Value v = json::Value::object();
    v["schema"] = "supple.recording/1.0";
    v["track"] = info.track.str();
    v["start_sample"] = static_cast<long long>(info.startSample);
    v["sample_rate"] = info.sampleRate;
    if (info.segments.size() > 1) {
        json::Value segs = json::Value::array();
        for (const RecordedSegment& sg : info.segments) {
            json::Value row = json::Value::array();
            row.push_back(static_cast<long long>(sg.fileFrame));
            row.push_back(static_cast<long long>(sg.timeline));
            segs.push_back(std::move(row));
        }
        v["segments"] = std::move(segs);
        json::Value loop = json::Value::array();
        loop.push_back(static_cast<long long>(info.loopStart));
        loop.push_back(static_cast<long long>(info.loopEnd));
        v["loop"] = std::move(loop);
    }
    return writeFileAtomic(sidecarPath(wav), json::writeCanonical(v) + "\n");
}

Result<RecordingInfo> readSidecar(const std::filesystem::path& wav) {
    auto text = readTextFile(sidecarPath(wav), 1 << 16);
    if (!text) return std::move(text).error();
    auto v = json::parse(*text);
    if (!v) return std::move(v).error();
    const json::Value* track = v->find("track");
    const json::Value* start = v->find("start_sample");
    const json::Value* rate = v->find("sample_rate");
    std::optional<model::Id> id =
        track && track->isString() ? model::Id::parse(track->asString()) : std::nullopt;
    if (!id || !start || !start->isInt() || !rate || !rate->isNumber())
        return Error("record.sidecar", sidecarPath(wav).string() + " is not a recording sidecar");
    RecordingInfo info{*id, start->asInt(), rate->asDouble(), {}, 0, 0};
    const json::Value* segs = v->find("segments");
    const json::Value* loop = v->find("loop");
    if (segs && segs->isArray() && loop && loop->isArray() && loop->size() == 2 &&
        loop->at(0).isInt() && loop->at(1).isInt()) {
        for (std::size_t i = 0; i < segs->size(); ++i) {
            const json::Value& row = segs->at(i);
            if (!row.isArray() || row.size() != 2 || !row.at(0).isInt() || !row.at(1).isInt())
                return Error("record.sidecar", sidecarPath(wav).string() + ": bad segments");
            info.segments.push_back({row.at(0).asInt(), row.at(1).asInt()});
        }
        info.loopStart = loop->at(0).asInt();
        info.loopEnd = loop->at(1).asInt();
    }
    return info;
}

void FrameRing::init(std::uint32_t channels, std::size_t frames) {
    channels_ = channels;
    capacity_ = frames;
    buf_.assign(frames * channels, 0.0f);
    write_ = 0;
    read_ = 0;
}

std::size_t FrameRing::write(const float* const* in, std::uint32_t inChannels,
                             std::span<const int> select, std::uint32_t frames) SUPPLE_RT {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t room = capacity_ - (w - read_.load(std::memory_order_acquire));
    const std::size_t n = std::min<std::size_t>(frames, room);
    for (std::size_t i = 0; i < n; ++i) {
        float* dst = buf_.data() + ((w + i) % capacity_) * channels_;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const int src = c < select.size() ? select[c] : -1;
            dst[c] = in && src >= 0 && static_cast<std::uint32_t>(src) < inChannels && in[src]
                         ? in[src][i]
                         : 0.0f;
        }
    }
    write_.store(w + n, std::memory_order_release);
    return n;
}

std::size_t FrameRing::read(float* out, std::size_t maxFrames) {
    const std::size_t r = read_.load(std::memory_order_relaxed);
    const std::size_t n = std::min(maxFrames, write_.load(std::memory_order_acquire) - r);
    for (std::size_t i = 0; i < n; ++i)
        std::copy_n(buf_.data() + ((r + i) % capacity_) * channels_, channels_,
                    out + i * channels_);
    read_.store(r + n, std::memory_order_release);
    return n;
}

Recorder::Recorder(std::vector<RecordTarget> targets, double rate, std::int64_t latency)
    : rate_(rate), latency_(latency) {
    for (RecordTarget& t : targets) {
        if (t.midi) {
            auto track = std::make_unique<MidiTrack>();
            track->target = std::move(t);
            midiTracks_.push_back(std::move(track));
            continue;
        }
        auto track = std::make_unique<Track>();
        track->target = std::move(t);
        track->ring.init(
            static_cast<std::uint32_t>(std::max<std::size_t>(1, track->target.inputs.size())),
            static_cast<std::size_t>(rate * 10.0));  // ten seconds
        tracks_.push_back(std::move(track));
    }
}

Recorder::~Recorder() {
    stop_ = true;
    if (writer_.joinable()) writer_.join();
}

Status Recorder::open() {
    for (auto& t : tracks_) {
        std::error_code ec;
        std::filesystem::create_directories(t->target.file.parent_path(), ec);
        SUPPLE_TRY(t->writer.open(t->target.file, rate_, t->ring.channels()));
    }
    writer_ = std::thread([this] { writerLoop(); });
    return okStatus();
}

void Recorder::capture(const float* const* in, std::uint32_t inChannels, std::uint32_t frames,
                       std::int64_t pos) SUPPLE_RT {
    if (frames == 0) return;
    if (!started_.load(std::memory_order_relaxed)) {
        // What the performer heard at `pos` reaches us a round trip later.
        start_.store(pos - latency_, std::memory_order_relaxed);
        started_.store(true, std::memory_order_release);
    } else if (pos != expected_) {  // a loop wrap: a new run on the timeline
        const std::size_t n = segmentCount_.load(std::memory_order_relaxed);
        if (n < kMaxSegments) {
            segments_[n] = {captured_, pos};
            segmentCount_.store(n + 1, std::memory_order_release);
        }
    }
    expected_ = pos + frames;
    captured_ += frames;
    for (auto& t : tracks_) {
        const std::size_t n = t->ring.write(in, inChannels, t->target.inputs, frames);
        if (n < frames) t->overflow.fetch_add(frames - n, std::memory_order_relaxed);
    }
}

void Recorder::captureMidi(std::span<const MidiInputEvent> events, std::uint32_t offset,
                           std::uint32_t frames, std::int64_t pos) SUPPLE_RT {
    (void)pos;  // placed by finish() from the captured position and the runs
    if (midiTracks_.empty() || events.empty() || !started_.load(std::memory_order_relaxed)) return;
    // capture() has counted this part: it began at captured_ - frames.
    const std::int64_t base = captured_ - frames;
    for (const MidiInputEvent& ev : events) {
        if (ev.frame < offset) continue;
        if (ev.frame >= offset + frames) break;
        for (auto& t : midiTracks_) {
            std::uint8_t m[3];
            if (!routeMidi(ev, t->target.midi->ports, t->target.midi->fromChannel,
                           t->target.midi->toChannel, m))
                continue;
            const std::size_t w = t->write.load(std::memory_order_relaxed);
            if (w - t->read.load(std::memory_order_acquire) >= MidiTrack::kRing) {
                t->overflow.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            MidiEvent& slot = t->ring[w % MidiTrack::kRing];
            slot.at = base + (ev.frame - offset);
            std::copy_n(m, 3, slot.bytes);
            t->write.store(w + 1, std::memory_order_release);
        }
    }
}

void Recorder::drainMidi() {
    for (auto& t : midiTracks_) {
        const std::size_t end = t->write.load(std::memory_order_acquire);
        for (std::size_t r = t->read.load(std::memory_order_relaxed); r != end; ++r)
            t->drained.push_back(t->ring[r % MidiTrack::kRing]);
        t->read.store(end, std::memory_order_release);
    }
}

void Recorder::lose(std::uint32_t frames) SUPPLE_RT {
    if (frames == 0 || !started_.load(std::memory_order_relaxed)) return;
    for (auto& t : tracks_) t->overflow.fetch_add(frames, std::memory_order_relaxed);
}

void Recorder::drain(Track& t, std::vector<float>& scratch) {
    const std::size_t chunk = scratch.size() / t.ring.channels();
    for (;;) {
        const std::size_t n = t.ring.read(scratch.data(), chunk);
        if (n == 0) break;
        if (writeError_) {
            auto st = t.writer.append(scratch.data(), n);
            if (!st) writeError_ = std::move(st);
        }
        if (!writeError_) t.overflow.fetch_add(n, std::memory_order_relaxed);  // lost too
    }
    if (writeError_) {
        auto st = t.writer.flush();  // a crash of the process keeps what the OS has
        if (!st) writeError_ = std::move(st);
    }
}

void Recorder::writeSidecars() {
    if (sidecars_ || !started_.load(std::memory_order_acquire)) return;
    sidecars_ = true;
    const std::int64_t start = start_.load(std::memory_order_relaxed);
    for (auto& t : tracks_) {
        // Without one the take is fine but a crash would leave it unplaced.
        auto st = writeSidecar(t->target.file, {t->target.track, start, rate_, {}, 0, 0});
        if (!st && sidecarError_) sidecarError_ = std::move(st);
    }
}

void Recorder::writerLoop() {
    std::vector<float> scratch(8192 * 16);
    auto lastHeader = std::chrono::steady_clock::now();
    while (!stop_.load(std::memory_order_acquire)) {
        writeSidecars();
        for (auto& t : tracks_) drain(*t, scratch);
        drainMidi();  // the MIDI rings too (bounded: a long performance must not overflow)
        if (std::chrono::steady_clock::now() - lastHeader > std::chrono::seconds(5)) {
            for (auto& t : tracks_) (void)t->writer.updateHeader();  // a crash keeps this much
            lastHeader = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    writeSidecars();
    for (auto& t : tracks_) drain(*t, scratch);  // what arrived before the stop
}

std::vector<RecordedTake> Recorder::finish(LoopRange loop) {
    stop_.store(true, std::memory_order_release);
    if (writer_.joinable()) writer_.join();
    std::vector<RecordedTake> out;
    const std::int64_t start =
        started_.load(std::memory_order_acquire) ? start_.load(std::memory_order_relaxed) : 0;
    // A run captured at output position P from file frame F was heard a
    // round trip earlier: its first frame on the timeline is F + latency.
    std::vector<RecordedSegment> segments = {{0, start}};
    const std::size_t count = segmentCount_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i)
        segments.push_back({segments_[i].fileFrame + latency_, segments_[i].timeline});
    // MIDI (ADR-0016): each event was played `latency` frames before it was
    // captured; the run holding that moment gives its timeline position.
    drainMidi();
    for (auto& t : midiTracks_) {
        RecordedTake r;
        r.track = t->target.track;
        r.isMidi = true;
        r.startSample = start;
        r.sampleRate = rate_;
        r.frames = captured_;
        for (const RecordedSegment& sg : segments)
            if (sg.fileFrame < r.frames) r.segments.push_back(sg);
        r.loop = {loop.start, loop.end};
        r.overflowFrames = t->overflow.load();
        for (const MidiEvent& ev : t->drained) {
            // ev.at counts captured frames like an audio file's frames; the
            // runs are in those units, already shifted by the latency.
            std::size_t k = 0;
            while (k + 1 < r.segments.size() && r.segments[k + 1].fileFrame <= ev.at) ++k;
            const RecordedSegment sg =
                r.segments.empty() ? RecordedSegment{0, start} : r.segments[k];
            RecordedMidiEvent e;
            e.timeline = sg.timeline + (ev.at - sg.fileFrame);
            e.run = static_cast<std::uint32_t>(k);
            std::copy_n(ev.bytes, 3, e.bytes);
            r.midi.push_back(e);
        }
        out.push_back(std::move(r));
    }
    for (auto& t : tracks_) {
        RecordedTake r;
        r.track = t->target.track;
        r.file = t->target.file;
        r.startSample = start;
        r.sampleRate = rate_;
        r.frames = static_cast<std::int64_t>(t->writer.frames());
        for (const RecordedSegment& sg : segments)
            if (sg.fileFrame < r.frames) r.segments.push_back(sg);
        r.loop = {loop.start, loop.end};
        r.overflowFrames = t->overflow.load();
        if (auto st = t->writer.close(); !st && writeError_) writeError_ = std::move(st);
        // The file's bytes are final now that it is closed, so hash them here:
        // the commit then never reads the recording back. (An incremental hash
        // of the write stream cannot be the file's digest — close() rewrites the
        // header's sizes — so this one read is what "the file's bytes" costs.)
        if (writeError_.ok()) {
            if (auto hash = sha256File(t->target.file)) r.hash = *hash;
        }
        if (r.segments.size() > 1 && started_.load(std::memory_order_relaxed)) {
            // Where each pass belongs, for recovery of an unsaved take.
            RecordingInfo info{t->target.track, start, rate_, r.segments, loop.start, loop.end};
            if (auto st = writeSidecar(t->target.file, info); !st && sidecarError_)
                sidecarError_ = std::move(st);
        }
        if (!writeError_)
            r.error = writeError_.error().toString();
        else if (!sidecarError_)
            r.error = sidecarError_.error().toString();
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace supple::engine::detail
