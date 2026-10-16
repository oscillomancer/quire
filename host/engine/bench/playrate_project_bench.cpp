// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Complete 30-track device-time graph, without devices or plugins. Preparation
// is excluded: the measured interval is one second of steady playback.
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>

#include "plan.h"
#include "supple/audio/audio_file.h"

using namespace supple;
using namespace supple::engine;
int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("supple-rate-bench-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::filesystem::remove_all(root); }
    } cleanup{root};
    audio::AudioData input;
    input.sampleRate = 48000;
    input.channels.assign(2, std::vector<float>(144000));
    for (std::size_t i = 0; i < input.frames(); ++i)
        input.channels[0][i] = input.channels[1][i] =
            static_cast<float>(.01 * std::sin(double(i) * .03));
    const auto file = root / "tone.wav";
    if (!audio::writeWav(file, input)) return 1;
    for (bool preserve : {false, true}) {
        model::EditTxn edit(model::Session::create(), 1);
        auto project = edit.project();
        project.playrate = {1.137, preserve};
        edit.setProject(project);
        for (int i = 0; i < 30; ++i) {
            model::Track track;
            track.parent = model::kMaster;
            const auto tr = edit.create(track);
            edit.modify<model::Track>(model::kMaster,
                                      [&](auto& master) { master.children.push_back(tr); });
            model::Take take;
            model::MediaSource source;
            source.path = file.string();
            source.format = "wav";
            source.sampleRate = 48000;
            source.channels = 2;
            source.frames = 144000;
            take.source = edit.create(source);
            const auto tk = edit.create(take);
            model::Item item;
            item.track = tr;
            item.takes = {tk};
            item.activeTake = tk;
            item.length = 6;
            edit.create(item);
        }
        auto session = edit.commit();
        if (!session) {
            std::cerr << session.error().toString();
            return 1;
        }
        InstancePool pool([](const model::PluginRef&) -> Result<host::PluginInfo> {
            return Error("bench.plugin", "no plugins");
        });
        MediaCache media;
        auto plan = detail::compile(*session, pool, 0, 64);
        if (!plan) {
            std::cerr << plan.error().toString();
            return 1;
        }
        plan->media = &media;
        if (!detail::prepare(*plan, *session) || !detail::prepareRateSources(*plan, 0, 0)) return 1;
        const auto start = std::chrono::steady_clock::now();
        double checksum = 0;
        for (std::int64_t device = 0; device < 48000; device += 64) {
            if (!detail::processBlock(*plan, device, 64)) return 1;
            checksum += double(plan->nodes[plan->mainMaster].post.l[0]);
        }
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << (preserve ? "stretch" : "tape")
                  << ": project_tracks=30 rate=1.137 audio_seconds=1 elapsed_seconds=" << elapsed
                  << " one_core_percent=" << elapsed * 100 << " checksum=" << checksum << '\n';
    }
}
