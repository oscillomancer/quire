// The skill's sound-design guide (skill/guides/sound-design.md): every JSON
// example is a valid patch, and every recipe renders finite, audible sound.
#include <doctest/doctest.h>

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
