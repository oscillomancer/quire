// Note-level MIDI editing (the piano roll's commands, and an agent's when
// notation is the wrong tool): midi.notes reads an item's notes and CCs in
// beats from the item's start, midi.edit applies ordered operations to them.
#include <algorithm>
#include <cmath>
#include <set>

#include "supple/engine/app.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using model::Id;

json::Value j(std::string_view s) { return json::parse(s).value(); }

struct ItemArgs {
    std::string item;
};

enum class OpKind : std::uint8_t { Add, Remove, Move, Resize, Velocity, Quantize, CcSet, CcClear };

struct CcPoint {
    double beat = 0;
    std::int64_t value = 0;
};

struct MidiOp {
    OpKind op = OpKind::Add;
    std::optional<std::vector<std::int64_t>> notes;  // indices in midi.notes order
    // add
    std::optional<std::int64_t> pitch, velocity, channel;
    std::optional<double> start, length;
    // move
    std::optional<double> beats;
    std::optional<std::int64_t> semitones;
    // resize: the end by length_delta, or the start by start_delta (the end stays)
    std::optional<double> lengthDelta, startDelta;
    // velocity: set (velocity) or delta
    std::optional<std::int64_t> velocityDelta;
    // quantize
    std::optional<double> grid, strength;
    std::optional<bool> ends;
    // cc.set / cc.clear
    std::optional<std::int64_t> number;
    std::optional<std::vector<CcPoint>> points;
    std::optional<double> from, to;
};

struct EditArgs {
    std::string item;
    std::vector<MidiOp> ops;
};

struct Frame {  // item beats <-> source ticks
    double ppq = 960, offset = 0, rate = 1, itemPosition = 0;
    double beatOf(std::int64_t tick) const {
        return (static_cast<double>(tick) / ppq - offset) / rate;
    }
    std::int64_t tickOf(double beat) const { return std::llround((beat * rate + offset) * ppq); }
    std::int64_t ticksOf(double beats) const { return std::llround(beats * rate * ppq); }
};

struct Target {
    Id item, take, source;
    const model::MidiSource* midi = nullptr;
    Frame frame;
};

Result<Target> midiTarget(const model::Session& s, const std::string& text) {
    auto id = Id::parse(text);
    const model::Item* it = id && id->kind == model::Kind::Item ? s.items.find(*id) : nullptr;
    if (!it) return Error("it.unknown", "no " + text, "/item", "use a live id from item.list");
    const model::Take* take = it->activeTake ? s.takes.find(*it->activeTake) : nullptr;
    if (!take || !take->source || take->source->kind != model::Kind::Midi)
        return Error("midi.not_midi", text + " has no MIDI take", "/item",
                     "use item.add with notation to create a MIDI item");
    const model::MidiSource* midi = s.midi.find(*take->source);
    if (!midi)
        return Error("mi.unknown", take->source->str() + " is missing from the model", "/item");
    Target t{*id, *it->activeTake, *take->source, midi, {}};
    t.frame.ppq = midi->sequence.ppq;
    t.frame.offset = take->startOffset;
    t.frame.rate = take->playrate > 0 ? take->playrate : 1.0;
    t.frame.itemPosition = model::itemSpan(s, *it).startBeat;
    return t;
}

double round4(double v) { return std::round(v * 1e4) / 1e4; }

json::Value noteJson(const midi::Note& n, const Frame& f) {
    json::Value v = json::Value::object();
    v["pitch"] = static_cast<std::int64_t>(n.pitch);
    v["start"] = round4(f.beatOf(n.start));
    v["length"] = round4(static_cast<double>(n.length) / f.ppq / f.rate);
    v["velocity"] = static_cast<std::int64_t>(n.velocity);
    if (n.channel) v["channel"] = static_cast<std::int64_t>(n.channel) + 1;
    return v;
}

Result<json::Value> describe(const model::Session& s, const Target& t) {
    const midi::Sequence& seq = t.midi->sequence;
    json::Value out = json::Value::object();
    out["item"] = t.item.str();
    out["ppq"] = static_cast<std::int64_t>(seq.ppq);
    out["length"] = round4(model::itemLengthBeats(s, *s.items.find(t.item)));
    out["position"] = round4(t.frame.itemPosition);
    if (t.midi->pooled) out["pooled"] = true;
    json::Value notes = json::Value::array();
    for (const midi::Note& n : seq.notes) notes.push_back(noteJson(n, t.frame));
    out["notes"] = std::move(notes);
    json::Value ccs = json::Value::array();
    for (const midi::Control& c : seq.controls) {
        if (c.kind != midi::ControlKind::CC) continue;
        json::Value v = json::Value::object();
        v["number"] = static_cast<std::int64_t>(c.number);
        v["beat"] = round4(t.frame.beatOf(c.tick));
        v["value"] = static_cast<std::int64_t>(c.value);
        if (c.channel) v["channel"] = static_cast<std::int64_t>(c.channel) + 1;
        ccs.push_back(std::move(v));
    }
    out["cc"] = std::move(ccs);
    return out;
}

// One working note: the original index (or -1 for an added note), alive, touched.
struct Working {
    midi::Note note;
    bool alive = true, touched = false;
};

Status applyOp(const MidiOp& op, std::size_t at, std::vector<Working>& notes, std::size_t original,
               midi::Sequence& seq, const Frame& f) {
    const std::string here = "/ops/" + std::to_string(at);
    constexpr std::int64_t tickLimit = std::int64_t{1} << 56;
    auto validBeat = [&](double beat, bool absolute) {
        const double ticks = (beat * f.rate + (absolute ? f.offset : 0)) * f.ppq;
        return std::isfinite(ticks) && std::abs(ticks) <= static_cast<double>(tickLimit);
    };
    const std::pair<const char*, std::optional<double>> numbers[] = {
        {"start", op.start},
        {"length", op.length},
        {"beats", op.beats},
        {"start_delta", op.startDelta},
        {"length_delta", op.lengthDelta},
        {"grid", op.grid},
        {"from", op.from},
        {"to", op.to},
        {"strength", op.strength}};
    for (const auto& [name, value] : numbers)
        if (value && !validBeat(*value, true))
            return Error("midi.range", "number exceeds the finite MIDI tick range",
                         here + "/" + name, "use a smaller finite number");
    if (op.grid && *op.grid < 1e-9)
        return Error("midi.range", "grid is too small", here + "/grid", "use at least 1e-9 beats");
    if ((op.semitones && (*op.semitones < -127 || *op.semitones > 127)) ||
        (op.velocityDelta && (*op.velocityDelta < -127 || *op.velocityDelta > 127)))
        return Error("midi.range", "pitch/velocity delta exceeds -127..127", here,
                     "use a delta within the MIDI range");
    for (const auto& n : notes)
        if (n.note.start < 0 || n.note.start > tickLimit || n.note.length < 0 ||
            n.note.length > tickLimit)
            return Error("midi.range", "source notes exceed the MIDI tick range", "/item",
                         "shorten the MIDI source");
    if (op.points)
        for (const auto& point : *op.points)
            if (!validBeat(point.beat, true))
                return Error("midi.range", "CC beat exceeds the finite MIDI tick range",
                             here + "/points");
    auto selection = [&](bool allWhenAbsent) -> Result<std::vector<std::size_t>> {
        std::vector<std::size_t> out;
        if (!op.notes) {
            if (!allWhenAbsent)
                return Error("midi.no_notes", "say which notes", here + "/notes",
                             "indices from midi.notes");
            for (std::size_t i = 0; i < original; ++i)
                if (notes[i].alive) out.push_back(i);
            return out;
        }
        for (std::int64_t i : *op.notes) {
            if (i < 0 || static_cast<std::size_t>(i) >= original)
                return Error("midi.note_index", "no note " + std::to_string(i), here + "/notes",
                             "indices are positions in midi.notes' list, 0 to " +
                                 std::to_string(original == 0 ? 0 : original - 1));
            if (notes[static_cast<std::size_t>(i)].alive)
                out.push_back(static_cast<std::size_t>(i));
        }
        return out;
    };
    const std::int64_t minLength = std::max<std::int64_t>(1, seq.ppq / 64);
    switch (op.op) {
        case OpKind::Add: {
            if (!op.pitch || !op.start || !op.length)
                return Error("midi.add_args", "add needs pitch, start and length", here,
                             R"({"op":"add","pitch":60,"start":0,"length":1})");
            midi::Note n;
            n.pitch = static_cast<std::uint8_t>(*op.pitch);
            n.start = f.tickOf(*op.start);
            n.length = std::max(minLength, f.ticksOf(*op.length));
            n.velocity = static_cast<std::uint8_t>(op.velocity.value_or(96));
            n.channel = static_cast<std::uint8_t>(op.channel.value_or(1) - 1);
            if (n.start < 0)
                return Error("midi.before_source", "the note starts before the item's MIDI",
                             here + "/start",
                             "start at " + std::to_string(round4(f.beatOf(0))) + " or later");
            notes.push_back({n, true, true});
            return {};
        }
        case OpKind::Remove: {
            auto sel = selection(false);
            if (!sel) return std::move(sel).error();
            for (std::size_t i : *sel) notes[i].alive = false;
            return {};
        }
        case OpKind::Move: {
            auto sel = selection(false);
            if (!sel) return std::move(sel).error();
            const std::int64_t dt = f.ticksOf(op.beats.value_or(0));
            const std::int64_t dp = op.semitones.value_or(0);
            for (std::size_t i : *sel) {
                midi::Note& n = notes[i].note;
                if (n.start + dt < 0)
                    return Error("midi.before_source", "a note would start before the item's MIDI",
                                 here + "/beats");
                if (n.pitch + dp < 0 || n.pitch + dp > 127)
                    return Error("midi.pitch_range", "a note would leave the MIDI range 0-127",
                                 here + "/semitones");
                n.start += dt;
                n.pitch = static_cast<std::uint8_t>(n.pitch + dp);
                notes[i].touched = true;
            }
            return {};
        }
        case OpKind::Resize: {
            auto sel = selection(false);
            if (!sel) return std::move(sel).error();
            for (std::size_t i : *sel) {
                midi::Note& n = notes[i].note;
                if (op.startDelta) {
                    const std::int64_t end = n.start + n.length;
                    n.start = std::clamp<std::int64_t>(n.start + f.ticksOf(*op.startDelta), 0,
                                                       std::max<std::int64_t>(0, end - minLength));
                    n.length = end - n.start;
                }
                if (op.lengthDelta)
                    n.length = std::max(minLength, n.length + f.ticksOf(*op.lengthDelta));
                if (op.length) n.length = std::max(minLength, f.ticksOf(*op.length));
                notes[i].touched = true;
            }
            return {};
        }
        case OpKind::Velocity: {
            auto sel = selection(false);
            if (!sel) return std::move(sel).error();
            for (std::size_t i : *sel) {
                midi::Note& n = notes[i].note;
                std::int64_t v = op.velocity ? *op.velocity : n.velocity;
                v += op.velocityDelta.value_or(0);
                n.velocity = static_cast<std::uint8_t>(std::clamp<std::int64_t>(v, 1, 127));
                notes[i].touched = true;
            }
            return {};
        }
        case OpKind::Quantize: {
            if (!op.grid || *op.grid <= 0)
                return Error("midi.grid", "quantize needs a grid in beats", here + "/grid",
                             "0.25 for sixteenths, 0.5 for eighths, 1/3 for triplet quarters");
            auto sel = selection(true);
            if (!sel) return std::move(sel).error();
            const double strength = std::clamp(op.strength.value_or(1.0), 0.0, 1.0);
            // The project's grid: snap project beats, not item beats.
            auto snap = [&](double itemBeat) {
                const double project = f.itemPosition + itemBeat;
                const double target = std::round(project / *op.grid) * *op.grid;
                return itemBeat + (target - project) * strength;
            };
            for (std::size_t i : *sel) {
                midi::Note& n = notes[i].note;
                const double start = f.beatOf(n.start);
                const double end = f.beatOf(n.start + n.length);
                if (!validBeat(snap(start), true) || !validBeat(snap(end), true))
                    return Error("midi.range", "quantized notes exceed the finite MIDI tick range",
                                 here + "/grid");
                const std::int64_t newStart = std::max<std::int64_t>(0, f.tickOf(snap(start)));
                std::int64_t newEnd =
                    op.ends.value_or(false) ? f.tickOf(snap(end)) : newStart + n.length;
                if (newEnd - newStart < minLength)
                    newEnd = newStart + std::max(minLength, f.ticksOf(*op.grid));
                n.start = newStart;
                n.length = newEnd - newStart;
                notes[i].touched = true;
            }
            return {};
        }
        case OpKind::CcSet: {
            if (!op.number || !op.points)
                return Error("midi.cc_args", "cc.set needs number and points", here,
                             R"({"op":"cc.set","number":1,"points":[{"beat":0,"value":64}]})");
            const auto channel = static_cast<std::uint8_t>(op.channel.value_or(1) - 1);
            for (const CcPoint& p : *op.points) {
                midi::Control c;
                c.kind = midi::ControlKind::CC;
                c.tick = f.tickOf(p.beat);
                c.channel = channel;
                c.number = static_cast<std::uint8_t>(*op.number);
                c.value = static_cast<std::int32_t>(std::clamp<std::int64_t>(p.value, 0, 127));
                if (c.tick < 0)
                    return Error("midi.before_source", "a point is before the item's MIDI", here);
                std::erase_if(seq.controls, [&](const midi::Control& x) {
                    return x.kind == c.kind && x.channel == c.channel && x.number == c.number &&
                           x.tick == c.tick;
                });
                seq.controls.push_back(c);
            }
            return {};
        }
        case OpKind::CcClear: {
            if (!op.number) return Error("midi.cc_args", "cc.clear needs number", here + "/number");
            const std::int64_t from =
                op.from ? f.tickOf(*op.from) : std::numeric_limits<std::int64_t>::min();
            const std::int64_t to =
                op.to ? f.tickOf(*op.to) : std::numeric_limits<std::int64_t>::max();
            std::erase_if(seq.controls, [&](const midi::Control& x) {
                return x.kind == midi::ControlKind::CC && x.number == *op.number &&
                       x.tick >= from && x.tick < to &&
                       (!op.channel || x.channel == *op.channel - 1);
            });
            return {};
        }
    }
    return {};
}

}  // namespace

void registerMidiCommands(cmd::Registry& registry) {
    {
        Command<ItemArgs, json::Value> c;
        c.name = "midi.notes";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List a MIDI item's notes and CC points (beats from the item's start).";
        c.description =
            "Notes are in a fixed order (by start, then pitch); midi.edit refers to them by their "
            "position in this list. start/length/beat are in beats from the item's start; channel "
            "is 1-16 (omitted for 1). pooled: other items share this MIDI, and edits change them "
            "too. For writing whole phrases, midi.write with notation is usually better.";
        c.tags = {"midi"};
        c.args =
            schema::object<ItemArgs>("midi_notes_args")
                .field("item", &ItemArgs::item, schema::string().pattern("^it_[0-9]+$", "item id"))
                .build();
        c.result = schema::any();
        c.examples = {
            {"Notes of a MIDI item", j(R"({"item": "it_1"})"),
             j(R"({"item": "it_1", "ppq": 960, "length": "<any>", "position": "<any>", "notes": "<any>", "cc": "<any>"})"),
             "song_with_item"}};
        c.run = [](const ItemArgs& a, Context& ctx) -> Result<json::Value> {
            const model::Session s = ctx.service<EngineApp>().view();
            auto t = midiTarget(s, a.item);
            if (!t) return std::move(t).error();
            return describe(s, *t);
        };
        registry.add(std::move(c));
    }
    {
        Command<EditArgs, json::Value> c;
        c.name = "midi.edit";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Edit a MIDI item's notes and CCs: add, remove, move, resize, velocity, quantize.";
        c.description =
            "ops run in order and are one undo step. notes: indices into midi.notes' list as it "
            "was before this command (every op refers to that list, so later ops are not "
            "affected by earlier removals). Beats are from the item's start. add {pitch, start, "
            "length, velocity?, channel?}; remove {notes}; move {notes, beats?, semitones?}; "
            "resize {notes, length? | length_delta? | start_delta?}; velocity {notes, velocity? "
            "and/or velocity_delta?}; quantize {notes? (all), grid (beats, on the project's grid), "
            "strength? 0-1, ends?}; cc.set {number, points [{beat, value}], channel?}; cc.clear "
            "{number, from?, to?}. Returns the new indices of the notes the ops added or changed "
            "(to keep a selection).";
        c.tags = {"midi"};
        auto point = schema::object<CcPoint>("cc_point")
                         .field("beat", &CcPoint::beat, schema::number())
                         .field("value", &CcPoint::value, schema::integer().range(0, 127))
                         .build();
        auto opSchema =
            schema::object<MidiOp>("midi_op")
                .field("op", &MidiOp::op,
                       schema::enumeration<OpKind>({{OpKind::Add, "add"},
                                                    {OpKind::Remove, "remove"},
                                                    {OpKind::Move, "move"},
                                                    {OpKind::Resize, "resize"},
                                                    {OpKind::Velocity, "velocity"},
                                                    {OpKind::Quantize, "quantize"},
                                                    {OpKind::CcSet, "cc.set"},
                                                    {OpKind::CcClear, "cc.clear"}}))
                .field("notes", &MidiOp::notes, schema::arrayOf(schema::integer().min(0)))
                .field("pitch", &MidiOp::pitch, schema::integer().range(0, 127))
                .field("start", &MidiOp::start, schema::number())
                .field("length", &MidiOp::length, schema::number().exclusiveMin(0))
                .field("velocity", &MidiOp::velocity, schema::integer().range(1, 127))
                .field("channel", &MidiOp::channel, schema::integer().range(1, 16))
                .field("beats", &MidiOp::beats, schema::number())
                .field("semitones", &MidiOp::semitones, schema::integer().range(-127, 127))
                .field("length_delta", &MidiOp::lengthDelta, schema::number())
                .field("start_delta", &MidiOp::startDelta, schema::number())
                .field("velocity_delta", &MidiOp::velocityDelta, schema::integer().range(-127, 127))
                .field("grid", &MidiOp::grid, schema::number().exclusiveMin(0))
                .field("strength", &MidiOp::strength, schema::number().range(0, 1))
                .field("ends", &MidiOp::ends, schema::boolean())
                .field("number", &MidiOp::number, schema::integer().range(0, 127))
                .field("points", &MidiOp::points, schema::arrayOf(point))
                .field("from", &MidiOp::from, schema::number())
                .field("to", &MidiOp::to, schema::number())
                .build();
        c.args =
            schema::object<EditArgs>("midi_edit_args")
                .field("item", &EditArgs::item, schema::string().pattern("^it_[0-9]+$", "item id"))
                .field("ops", &EditArgs::ops, schema::arrayOf(opSchema))
                .build();
        c.result = schema::any();
        c.examples = {
            {"Add a note and move the first one up an octave",
             j(R"({"item": "it_1", "ops": [{"op": "add", "pitch": 64, "start": 0.5, "length": 0.5}, {"op": "move", "notes": [0], "semitones": 12}]})"),
             j(R"({"notes": "<any>"})"), "song_with_item"}};
        c.run = [](const EditArgs& a, Context& ctx) -> Result<json::Value> {
            EngineApp& app = ctx.service<EngineApp>();
            const model::Session s = app.view();
            auto t = midiTarget(s, a.item);
            if (!t) return std::move(t).error();
            midi::Sequence seq = t->midi->sequence;
            std::vector<Working> notes;
            notes.reserve(seq.notes.size());
            for (const midi::Note& n : seq.notes) notes.push_back({n, true, false});
            const std::size_t original = notes.size();
            for (std::size_t i = 0; i < a.ops.size(); ++i)
                SUPPLE_TRY(applyOp(a.ops[i], i, notes, original, seq, t->frame));
            seq.notes.clear();
            std::vector<midi::Note> touched;
            for (const Working& w : notes)
                if (w.alive) {
                    seq.notes.push_back(w.note);
                    if (w.touched) touched.push_back(w.note);
                    seq.length = std::max(seq.length, w.note.start + w.note.length);
                }
            seq.canonicalize();
            json::Value selected = json::Value::array();
            std::set<std::size_t> seen;
            for (const midi::Note& n : touched) {
                const auto it = std::find(seq.notes.begin(), seq.notes.end(), n);
                if (it != seq.notes.end())
                    seen.insert(static_cast<std::size_t>(it - seq.notes.begin()));
            }
            for (std::size_t i : seen) selected.push_back(static_cast<std::int64_t>(i));
            model::MidiSource src = *t->midi;
            src.sequence = std::move(seq);
            app.txn().put<model::MidiSource>(t->source, std::move(src));
            json::Value out = json::Value::object();
            out["notes"] = std::move(selected);
            return out;
        };
        registry.add(std::move(c));
    }
}

}  // namespace supple::engine
