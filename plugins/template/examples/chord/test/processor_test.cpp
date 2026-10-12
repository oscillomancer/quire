// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's own claims, measured through the Engine the plugin runs.
#include <algorithm>
#include <random>
#include <vector>

#include "Check.h"
#include "Params.h"
#include "Processor.h"
#include "Properties.h"
#include "Testing.h"

using namespace supple;
using namespace product;

namespace {

using Kind = NoteOutput::Kind;

std::vector<test::Sent> play(std::vector<test::EventAt> events, std::vector<std::pair<uint32_t, double>> initial = {},
                             std::vector<int> blocks = {512}, int frames = 48000,
                             std::vector<std::pair<uint32_t, std::string>> properties = {}) {
    std::vector<test::Sent> sent;
    test::RenderOptions o;
    o.properties = std::move(properties);
    o.frames = frames;
    o.blocks = std::move(blocks);
    o.initial = std::move(initial);
    o.events = std::move(events);
    o.sent = &sent;
    test::render({}, o);
    return sent;
}

std::vector<test::Sent> only(const std::vector<test::Sent>& all, Kind kind, NoteEvent::Type type = NoteEvent::Type::On) {
    std::vector<test::Sent> out;
    for (const auto& s : all)
        if (s.kind == kind && (kind != Kind::Note || s.note.type == type)) out.push_back(s);
    return out;
}

}  // namespace

TEST(a_note_leaves_as_its_chord_on_its_own_frame) {
    const auto sent = play({test::noteOn(100, 60, 0.7, 5)}, {{kShape, 2.0}});  // major
    const auto ons = only(sent, Kind::Note);
    CHECK(ons.size() == 3);
    const int keys[] = {60, 64, 67};
    for (size_t k = 0; k < ons.size() && k < 3; ++k) {
        CHECK(ons[k].frame == 100 && ons[k].note.key == keys[k] && ons[k].note.velocity == 0.7);
    }
    CHECK(!ons.empty() && ons[0].note.noteId == 5);  // the root keeps the host's ID
}

TEST(a_strum_lands_on_exact_frames_whatever_the_blocks) {
    // 10 ms at 48 kHz: 480 frames apart, the seventh chord's four notes.
    const std::vector<test::EventAt> in = {test::noteOn(100, 48)};
    const std::vector<std::pair<uint32_t, double>> set = {{kShape, 5.0}, {kStrum, 10.0}};
    const auto a = only(play(in, set, {4096}), Kind::Note), b = only(play(in, set, {1, 13, 64, 1000}), Kind::Note);
    CHECK(a.size() == 4 && b.size() == 4);
    for (size_t k = 0; k < a.size() && k < b.size(); ++k) {
        CHECK(a[k].frame == 100 + 480 * static_cast<int>(k));
        CHECK(a[k].frame == b[k].frame && a[k].note.key == b[k].note.key);
    }
}

TEST(a_release_ends_what_was_sent_and_retires_the_input_note) {
    const auto sent = play({test::noteOn(0, 60, 1.0, 9), test::noteOff(5000, 60, 9)}, {{kShape, 3.0}});  // minor
    const auto offs = only(sent, Kind::Note, NoteEvent::Type::Off);
    CHECK(offs.size() == 3);
    for (const auto& o : offs) CHECK(o.frame == 5000);
    const auto ends = only(sent, Kind::End);
    CHECK(ends.size() == 1 && ends[0].frame == 5000 && ends[0].note.noteId == 9 && ends[0].note.key == 60);
}

TEST(a_release_mid_strum_sends_no_more) {
    // 50 ms strum (2400 frames), released at 3000: two of three notes started.
    const auto sent = play({test::noteOn(0, 60), test::noteOff(3000, 60)}, {{kShape, 2.0}, {kStrum, 50.0}});
    CHECK(only(sent, Kind::Note).size() == 2);
    CHECK(only(sent, Kind::Note, NoteEvent::Type::Off).size() == 2);
}

TEST(a_note_beyond_the_chord_capacity_is_retired_immediately) {
    std::vector<test::EventAt> events;
    for (int id = 0; id <= Processor::kChords; ++id) {
        events.push_back(test::noteOn(id, 60, 1.0, id));
        events.push_back(test::noteOff(1000 + id, 60, id));
    }
    const auto sent = play(events, {{kShape, 2.0}}, {64}, 2000);
    const auto ends = only(sent, Kind::End);
    CHECK(ends.size() == Processor::kChords + 1);
    int rejected = 0;
    for (const auto& end : ends) {
        if (end.note.noteId != Processor::kChords) continue;
        ++rejected;
        CHECK(end.frame == Processor::kChords);
    }
    CHECK(rejected == 1);
    CHECK(only(sent, Kind::Note).size() == 3 * Processor::kChords);
    CHECK(only(sent, Kind::Note, NoteEvent::Type::Off).size() == 3 * Processor::kChords);
}

TEST(other_midi_passes_through_on_its_frame) {
    const auto sent = play({test::midi(10, MidiMessage::cc(3, 1, 64)), test::midi(700, MidiMessage::bend(0, 0.5))});
    const auto midi = only(sent, Kind::Midi);
    CHECK(midi.size() == 2);
    CHECK(midi.size() == 2 && midi[0].frame == 10 && midi[0].midi.isController() && midi[0].midi.channel() == 3 &&
          midi[1].frame == 700 && midi[1].midi.isPitchBend());
}

TEST(every_note_started_is_stopped) {
    std::mt19937 rng(4);
    std::vector<test::EventAt> in;
    for (int k = 0; k < 200; ++k) {
        const int start = static_cast<int>(rng() % 90000), key = 36 + static_cast<int>(rng() % 48);
        in.push_back(test::noteOn(start, key, 1.0, k));
        in.push_back(test::noteOff(start + 1 + static_cast<int>(rng() % 8000), key, k));
    }
    const auto sent = play(in, {{kShape, 5.0}, {kStrum, 20.0}}, {1, 333, 2048}, 100000);
    int balance = 0;
    for (const auto& s : sent) {
        if (s.kind != Kind::Note) continue;
        balance += s.note.type == NoteEvent::Type::On ? 1 : -1;
        CHECK(balance >= 0);
    }
    CHECK_MSG(balance == 0, "%d notes left sounding", balance);
    CHECK(only(sent, Kind::End).size() == 200);
}


TEST(the_custom_shape_plays_the_intervals_typed) {
    // Commas or spaces; "x" and 99 (out of range) are skipped; strum order kept.
    const auto ons = only(play({test::noteOn(10, 60, 0.5, 3)}, {{kShape, kCustomShape}}, {512}, 4800,
                               {{kIntervals, "0, 3 x 7 99 -12"}}),
                          Kind::Note);
    const int keys[] = {60, 63, 67, 48};
    CHECK(ons.size() == 4);
    for (size_t k = 0; k < ons.size() && k < 4; ++k) CHECK(ons[k].note.key == keys[k] && ons[k].frame == 10);
    CHECK(!ons.empty() && ons[0].note.noteId == 3);
}

TEST(custom_text_with_nothing_usable_plays_the_note_alone) {
    for (const char* text : {"", "chord", "100 -30", ", ,"}) {
        const auto ons = only(play({test::noteOn(0, 60)}, {{kShape, kCustomShape}}, {512}, 4800, {{kIntervals, text}}),
                              Kind::Note);
        CHECK_MSG(ons.size() == 1 && ons[0].note.key == 60, "'%s' played %zu notes", text, ons.size());
    }
    const Processor::Shape s = Processor::parseIntervals("0 1 2 3 4 5 6 7 8");
    CHECK(s.count == Processor::kMaxNotes);
}

TEST(notes_beyond_the_keyboard_are_left_out_of_the_chord) {
    const auto ons = only(play({test::noteOn(0, 120, 1.0, 4)}, {{kShape, kCustomShape}}, {512}, 4800,
                               {{kIntervals, "12 0 4"}}),
                          Kind::Note);
    // 132 does not exist; the root that does keeps the host's ID.
    CHECK(ons.size() == 2 && ons[0].note.key == 120 && ons[0].note.noteId == 4 && ons[1].note.key == 124);
}

TEST(every_key_is_named_by_the_chord_it_plays) {
    ParamValues v;
    NoteNames names;
    Processor::noteNames(v, names);
    CHECK(names.size() == 128);
    CHECK(names[60].key == 60 && names[60].name == "C4 Major" && names[60].channel == -1);
    CHECK(names[0].name == "C-1 Major" && names[127].name == "G9 Major");
    v.setIndex(static_cast<size_t>(params::indexOf(kShape)), 5.0);
    NoteNames sevenths;
    Processor::noteNames(v, sevenths);
    CHECK(sevenths[69].name == "A4 7th" && !(sevenths == names));
}

SUPPLE_TEST_MAIN
