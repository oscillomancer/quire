// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// The skill's sound-design guide (skill/guides/sound-design.md): every JSON
// example is a valid patch, and every recipe renders finite, audible sound.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "orrery/engine.h"
#include "orrery/patch.h"

using namespace supple;
using namespace supple::orrery;

namespace {

std::vector<std::string> jsonBlocks(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while ((at = text.find("```json\n", at)) != std::string::npos) {
        at += 8;
        const std::size_t end = text.find("```", at);
        if (end == std::string::npos) break;
        out.push_back(text.substr(at, end - at));
        at = end + 3;
    }
    return out;
}

double renderRms(const Patch& p) {
    Engine engine;
    engine.prepare(48000.0, 256);
    engine.setPatch(p);
    engine.reset();
    std::vector<float> l(256), r(256);
    std::vector<Event> on;
    for (int k : {48, 55, 60}) {
        Event e;
        e.type = Event::Type::NoteOn;
        e.key = static_cast<std::int16_t>(k);
        e.value = 0.8;
        on.push_back(e);
    }
    double sum = 0.0;
    std::size_t count = 0;
    for (int b = 0; b < 190; ++b) {  // ~1 s
        engine.process(b == 0 ? std::span<const Event>(on) : std::span<const Event>(), Transport{},
                       l.data(), r.data(), 256);
        for (std::size_t i = 0; i < 256; ++i) {
            REQUIRE(std::isfinite(l[i]));
            REQUIRE(std::isfinite(r[i]));
            sum += static_cast<double>(l[i]) * static_cast<double>(l[i]);
            ++count;
        }
    }
    return std::sqrt(sum / static_cast<double>(count));
}

std::vector<float> renderPerformance(const Patch& patch, std::uint32_t block) {
    constexpr std::uint32_t frames = 24000;
    Engine engine;
    engine.prepare(48000.0, block);
    engine.setPatch(patch);
    engine.reset();
    std::vector<Event> timed;
    for (std::uint32_t at : {99u, 12007u, 21013u})
        for (int key : {48, 52, 55, 59, 62, 64, 67, 71}) {
            Event e;
            e.type = at == 99 ? Event::Type::NoteOn : Event::Type::NoteOff;
            e.time = at;
            e.key = static_cast<std::int16_t>(key);
            e.value = 0.8;
            timed.push_back(e);
        }
    // Exercise mono -> stereo filter-state inheritance and return to centre,
    // with control ramps split at unrelated host boundaries.
    for (std::uint32_t at : {5003u, 8009u}) {
        Event e;
        e.type = Event::Type::Param;
        e.index = static_cast<std::uint32_t>(index(kOsc1, OscF::pan));
        e.value = at == 5003 ? 80.0 : 0.0;
        e.time = at;
        timed.push_back(e);
    }
    std::stable_sort(timed.begin(), timed.end(),
                     [](const Event& a, const Event& b) { return a.time < b.time; });
    std::vector<float> result(frames * 2), left(block), right(block);
    std::vector<Event> events;
    std::size_t next = 0;
    for (std::uint32_t at = 0; at < frames; at += block) {
        const auto n = std::min(block, frames - at);
        events.clear();
        while (next < timed.size() && timed[next].time < at + n) {
            Event e = timed[next++];
            e.time -= at;
            events.push_back(e);
        }
        engine.process(events, Transport{}, left.data(), right.data(), n);
        for (std::uint32_t s = 0; s < n; ++s) {
            result[2 * (at + s)] = left[s];
            result[2 * (at + s) + 1] = right[s];
        }
    }
    return result;
}

}  // namespace

TEST_CASE("every patch in the sound-design guide parses and renders") {
    std::ifstream in(std::string(SUPPLE_SOURCE_DIR) + "/skill/guides/sound-design.md");
    REQUIRE(in);
    std::stringstream text;
    text << in.rdbuf();
    const auto blocks = jsonBlocks(text.str());
    REQUIRE(blocks.size() >= 11);  // the anatomy example and ten recipes
    const PatchType type;
    for (const std::string& b : blocks) {
        INFO(b.substr(0, 120));
        auto p = type.parse(b);
        REQUIRE_MESSAGE(p, (p ? "" : p.error().toString()));
        CHECK(renderRms(*p) > 1e-3);
    }
}

TEST_CASE("every guide recipe remains bit-identical across host block sizes") {
    std::ifstream in(std::string(SUPPLE_SOURCE_DIR) + "/skill/guides/sound-design.md");
    REQUIRE(in);
    std::stringstream text;
    text << in.rdbuf();
    const PatchType type;
    for (const auto& json : jsonBlocks(text.str())) {
        INFO(json.substr(0, 120));
        const auto patch = type.parse(json);
        REQUIRE(patch);
        const auto reference = renderPerformance(*patch, 4096);
        for (std::uint32_t block : {7u, 256u}) {
            CAPTURE(block);
            CHECK(renderPerformance(*patch, block) == reference);
        }
    }
}
