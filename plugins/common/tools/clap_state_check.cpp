// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// clap-state-check <plugin.clap> [plugin-id]
//
// Saved state is untrusted input: a host hands a plugin whatever bytes a
// project, a preset or a shared file holds, cut short, corrupted or made by
// another version. The template fuzzes state for products with tables; a
// run-time product (every stock script) has its own Definition, its layer's
// chunk and whatever its script reads from them, which nothing else damages.
//
// This saves states the plugin makes (its defaults, seeded random settings
// and some of its factory patches, which carry what parameters cannot: a
// synth's routes), damages each a few hundred ways from a fixed seed (bits
// flipped, cut short, bytes inserted, a length replaced with an awkward
// number, a run repeated) and loads every one, requiring that:
//
//   - nothing crashes, and no sanitizer has anything to say;
//   - a refused load changes nothing: the state saves as it did before;
//   - an accepted one saves, loads into a fresh instance and saves again
//     byte for byte;
//   - a plugin that accepted it processes finite audio.
//
// Every stock plugin registers this as plugin.<id>.state. A failure names
// its seed state and damage number, so it always reproduces.
// Exit 0 = every plugin in the file passed.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "supple/host/clap_host.h"
#include "supple/json/json.h"
#include "supple/json/write.h"

using namespace supple;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::uint32_t kBlock = 512;
constexpr int kBlocks = 4;
constexpr int kDamagesPerSeed = 600;
constexpr int kRandomSettings = 2;
constexpr std::size_t kFactoryPatches = 4;
constexpr int kReported = 5;  // failures printed in full; the rest are counted

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

using Bytes = std::vector<std::uint8_t>;

struct Seed {
    std::string name;
    Bytes state;
};

// One state damaged: one to four edits, each a bit flipped, the state cut
// short, a byte inserted, four bytes replaced with an awkward number (a
// length or a count, often) or a run repeated.
Bytes damage(Bytes x, std::mt19937& rng) {
    const int edits = 1 + static_cast<int>(rng() % 4);
    for (int e = 0; e < edits && !x.empty(); ++e) {
        const std::size_t at = rng() % x.size();
        switch (rng() % 5) {
            case 0:
                x[at] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
                break;
            case 1:
                x.resize(at);
                break;
            case 2:
                x.insert(x.begin() + static_cast<std::ptrdiff_t>(at),
                         static_cast<std::uint8_t>(rng()));
                break;
            case 3: {
                static const std::uint32_t awkward[] = {0u,          1u,  0x7fffffffu, 0xffffffffu,
                                                        0x80000000u, 12u, 255u,        65536u};
                const std::uint32_t v = awkward[rng() % 8];
                for (std::size_t k = 0; k < 4 && at + k < x.size(); ++k)
                    x[at + k] = static_cast<std::uint8_t>(v >> (8 * k));
                break;
            }
            default: {
                const std::size_t length = std::min<std::size_t>(1 + rng() % 24, x.size() - at);
                const Bytes run(x.begin() + static_cast<std::ptrdiff_t>(at),
                                x.begin() + static_cast<std::ptrdiff_t>(at + length));
                x.insert(x.begin() + static_cast<std::ptrdiff_t>(at), run.begin(), run.end());
            }
        }
    }
    return x;
}

// The states the plugin makes: its defaults, seeded random settings of every
// writable parameter, and factory patches spread across its list.
std::vector<Seed> makeSeeds(const host::PluginInfo& info) {
    std::vector<Seed> seeds;
    auto in = host::Instance::create(info, {});
    if (!in) return seeds;
    auto save = [&](std::string name) {
        if (auto s = (*in)->saveState()) seeds.push_back({std::move(name), std::move(*s)});
    };
    save("defaults");

    std::vector<host::ParamInfo> params;
    for (const auto& p : (*in)->params())
        if (!(p.flags & CLAP_PARAM_IS_READONLY)) params.push_back(p);
    std::mt19937 rng(0x57a7eu);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int k = 0; k < kRandomSettings && !params.empty(); ++k) {
        for (const auto& p : params) {
            double v = p.min + (p.max - p.min) * unit(rng);
            if (p.flags & CLAP_PARAM_IS_STEPPED) v = std::clamp(std::round(v), p.min, p.max);
            (void)(*in)->setParam(p.id, v);
        }
        save("random settings " + std::to_string(k + 1));
    }

    if (info.semantic) {
        if (auto patches = (*in)->factoryPatches(); patches && patches->isArray()) {
            const auto& list = patches->asArray();
            const std::size_t n = std::min(list.size(), kFactoryPatches);
            for (std::size_t k = 0; k < n; ++k) {
                const json::Value& patch = list[k * list.size() / n];
                const json::Value* name = patch.find("name");
                if ((*in)->setPatch(json::writeCanonical(patch), CLAP_SUPPLE_SET_REPLACE).ok())
                    save("factory patch " + (name && name->isString() ? name->asString() : "?"));
            }
        }
    }
    return seeds;
}

// Seeded noise, louder on the left, so a processor has something to work on.
bool processesFinite(host::Instance& in, std::mt19937& rng) {
    if (in.restartPending()) {
        in.deactivate();
        if (!in.activate(kSampleRate, 1, kBlock).ok()) return false;
    }
    std::vector<float> outL(kBlock), outR(kBlock), inL(kBlock), inR(kBlock);
    float* outPtr[2] = {outL.data(), outR.data()};
    float* inPtr[2] = {inL.data(), inR.data()};
    std::vector<host::OutputEvent> storage(256);
    host::OutputEvents outEvents;
    outEvents.storage = storage;
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
    const auto inChannels = in.audioInputChannels();
    const auto outChannels = in.audioOutputChannels();
    if (!in.startProcessing()) return false;
    bool ok = true;
    for (int b = 0; b < kBlocks && ok; ++b) {
        for (std::uint32_t i = 0; i < kBlock; ++i) {
            inL[i] = noise(rng);
            inR[i] = 0.5f * noise(rng);
        }
        host::ProcessBlock block;
        block.inputs = inChannels > 0 ? inPtr : nullptr;
        block.inputChannels = std::min<std::uint32_t>(inChannels, 2u);
        block.outputs = outPtr;
        block.outputChannels = std::min<std::uint32_t>(outChannels, 2u);
        block.frames = kBlock;
        block.steadyTime = static_cast<std::int64_t>(b) * kBlock;
        outEvents.clear();
        ok = in.process(block, &outEvents);
        for (std::uint32_t c = 0; c < block.outputChannels && ok; ++c)
            for (std::uint32_t i = 0; i < kBlock; ++i)
                if (!std::isfinite(outPtr[c][i])) ok = false;
    }
    in.stopProcessing();
    return ok;
}

void testPlugin(const host::PluginInfo& info) {
    std::printf("%s\n", info.id.c_str());
    const std::vector<Seed> seeds = makeSeeds(info);
    auto a = host::Instance::create(info, {});
    auto player = host::Instance::create(info, {});
    if (seeds.empty() || !a || !player || !(*player)->activate(kSampleRate, 1, kBlock).ok()) {
        check(false, "create, activate and save a state");
        return;
    }

    int loads = 0, accepted = 0, reported = 0, failed = 0;
    auto fail = [&](const std::string& what) {
        ++failed;
        if (reported++ < kReported) std::printf("     %s\n", what.c_str());
    };
    std::mt19937 noise(0x5e7au);
    for (std::size_t s = 0; s < seeds.size(); ++s) {
        std::mt19937 rng(0xda7a + static_cast<std::uint32_t>(s));
        for (int d = -1; d < kDamagesPerSeed; ++d) {
            // -1 is the seed itself, undamaged, which must load.
            const Bytes input = d < 0 ? seeds[s].state : damage(seeds[s].state, rng);
            const std::string where =
                seeds[s].name + (d < 0 ? "" : ", damage " + std::to_string(d));
            ++loads;
            auto before = (*a)->saveState();
            if (!before) {
                fail(where + ": the plugin could not save its state before the load");
                continue;
            }
            if (!(*a)->loadState(input).ok()) {
                if (d < 0) fail(where + ": the plugin refused its own state");
                auto after = (*a)->saveState();
                if (!after || *after != *before) fail(where + ": a refused load changed the state");
                continue;
            }
            ++accepted;
            auto once = (*a)->saveState();
            if (!once) {
                fail(where + ": an accepted state did not save");
                continue;
            }
            auto b = host::Instance::create(info, {});
            if (!b || !(*b)->loadState(*once).ok()) {
                fail(where + ": the plugin refused a state it saved");
                continue;
            }
            auto twice = (*b)->saveState();
            if (!twice || *twice != *once)
                fail(where + ": a loaded state did not save again byte for byte");
            if (!(*player)->loadState(*once).ok() || !processesFinite(**player, noise))
                fail(where + ": the accepted state did not process finite audio");
        }
    }
    char detail[192];
    std::snprintf(detail, sizeof(detail), "%zu seed states, %d loads, %d accepted, %d failures",
                  seeds.size(), loads, accepted, failed);
    check(failed == 0,
          std::string("damaged state is refused cleanly or accepted whole: ") + detail);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <plugin.clap> [plugin-id]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path path = argv[1];
    const std::string only = argc == 3 ? argv[2] : "";
    auto list = host::scan(std::span<const std::filesystem::path>(&path, 1));
    if (!list) {
        std::fprintf(stderr, "scan failed: %s\n", list.error().toString().c_str());
        return 2;
    }
    int tested = 0;
    for (const auto& info : *list) {
        if (!only.empty() && info.id != only) continue;
        ++tested;
        testPlugin(info);
    }
    if (tested == 0) {
        std::fprintf(stderr, "no plugin found in %s\n", path.string().c_str());
        return 2;
    }
    std::printf("%s: %d failure(s)\n", path.filename().string().c_str(), failures);
    return failures ? 1 : 0;
}
