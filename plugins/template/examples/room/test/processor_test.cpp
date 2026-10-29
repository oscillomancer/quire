// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's own claims, measured through the Engine the plugin runs:
// the room it builds, a file's response played exactly, the factory plate
// played as the same file would be, a missing file falling back to the room,
// and a change of room without a click.
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "Check.h"
#include "Params.h"
#include "Processor.h"
#include "Properties.h"
#include "Testing.h"
#include "core/Files.h"
#include "dsp/AudioFile.h"

using namespace supple;
using namespace product;

namespace {

constexpr double kRate = 48000.0;

test::Signal impulse(int frames) {
    test::Signal s = test::silence(2, frames);
    s[0][0] = s[1][0] = 1.0;
    return s;
}

// A stereo response in a temporary WAV: left 1 at 0 and 0.5 at 100, right
// 1 at 50.
struct ResponseFile {
    std::filesystem::path folder;
    std::string path;
    ResponseFile() {
        folder = std::filesystem::temp_directory_path() /
                 ("supple-room-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(folder);
        path = (folder / "response.wav").string();
        dsp::AudioData a;
        a.sampleRate = kRate;
        a.channel.assign(2, std::vector<float>(200, 0.0f));
        a.channel[0][0] = 1.0f;
        a.channel[0][100] = 0.5f;
        a.channel[1][50] = 1.0f;
        dsp::saveWavFile(path, a);
    }
    ~ResponseFile() {
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
    }
};

test::RenderOptions wetOnly() {
    test::RenderOptions o;
    o.initial = {{kMix, 1.0}, {kPredelay, 0.0}, {kTone, 20000.0}, {kOutput, 0.0}};
    return o;
}

}  // namespace

TEST(the_built_in_room_is_wide_decays_and_is_the_same_every_time) {
    const test::RenderOptions o = wetOnly();
    const test::Signal a = test::render(impulse(96000), o), b = test::render(impulse(96000), o);
    CHECK(test::maxDifference(a, b) == 0.0);
    // Sound for its length, then none.
    const int length = static_cast<int>(Processor::kRoomSeconds * kRate);
    CHECK(test::rms(test::Signal{{a[0].begin(), a[0].begin() + 4800}}) > 1e-3);
    CHECK(test::peak(a, length + 10) < 1e-9);
    // Its two channels are different rooms.
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (int n = 0; n < length; ++n) {
        lr += a[0][static_cast<size_t>(n)] * a[1][static_cast<size_t>(n)];
        ll += a[0][static_cast<size_t>(n)] * a[0][static_cast<size_t>(n)];
        rr += a[1][static_cast<size_t>(n)] * a[1][static_cast<size_t>(n)];
    }
    CHECK(std::fabs(lr / std::sqrt(ll * rr)) < 0.2);
    CHECK(test::allFinite(a));
}

TEST(no_mix_is_the_dry_input_exactly) {
    test::RenderOptions o;
    o.initial = {{kMix, 0.0}, {kOutput, 0.0}};
    const test::Signal in = test::noise(2, 9600);
    CHECK(test::maxDifference(test::render(in, o), in) == 0.0);
}

TEST(a_files_response_plays_exactly_scaled_to_unit_energy_and_pre_delayed) {
    ResponseFile file;
    test::RenderOptions o = wetOnly();
    o.properties = {{kImpulse, file.path}};
    const test::Signal out = test::render(impulse(1000), o);
    const double scale = 1.0 / std::sqrt(1.25);  // the louder channel's energy: 1 + 0.25
    CHECK_NEAR(out[0][0], scale, 1e-6);
    CHECK_NEAR(out[0][100], 0.5 * scale, 1e-6);
    CHECK_NEAR(out[1][50], scale, 1e-6);
    CHECK_NEAR(out[1][0], 0.0, 1e-6);
    CHECK(test::peak(out, 201) < 1e-6);
    // Ten milliseconds of pre-delay: 480 samples later, the same.
    o.initial.push_back({kPredelay, 10.0});
    const test::Signal later = test::render(impulse(1000), o);
    CHECK_NEAR(later[0][480], scale, 1e-6);
    CHECK_NEAR(later[0][580], 0.5 * scale, 1e-6);
    CHECK(test::peak(test::Signal{{later[0].begin(), later[0].begin() + 480}}) < 1e-9);
}

TEST(the_factory_plate_plays_as_the_same_file_on_disk_does) {
    const assets::Asset plate = files::factoryFile("factory:plate.flac");
    CHECK(plate);
    if (!plate) return;
    ResponseFile file;
    const std::string copy = (file.folder / "plate.flac").string();
    std::ofstream(copy, std::ios::binary).write(reinterpret_cast<const char*>(plate.data),
                                                static_cast<std::streamsize>(plate.size));
    test::RenderOptions factory = wetOnly(), disk = wetOnly(), room = wetOnly();
    factory.properties = {{kImpulse, "factory:plate.flac"}};
    disk.properties = {{kImpulse, copy}};
    const test::Signal out = test::render(impulse(19200), factory);
    CHECK(test::maxDifference(out, test::render(impulse(19200), disk)) == 0.0);
    CHECK(test::maxDifference(out, test::render(impulse(19200), room)) > 0.01);
}

TEST(a_missing_file_plays_the_built_in_room) {
    test::RenderOptions o = wetOnly();
    o.properties = {{kImpulse, "/nowhere/this/does/not/exist.wav"}};
    test::RenderOptions room = wetOnly();
    CHECK(test::maxDifference(test::render(impulse(9600), o), test::render(impulse(9600), room)) == 0.0);
}

TEST(changing_the_room_while_it_plays_does_not_click) {
    ResponseFile file;
    auto engine = std::make_unique<Engine>();
    for (const auto& [id, plain] : wetOnly().initial)
        engine->store().set(static_cast<size_t>(params::indexOf(id)), toHost(*params::find(id), plain));
    engine->activate(kRate, 256, 2, 2);
    const int k = properties::indexOf(kImpulse);
    std::vector<double> l(256), r(256), out;
    double* io[2] = {l.data(), r.data()};
    double phase = 0.0;
    auto block = [&] {
        for (int i = 0; i < 256; ++i, phase += 200.0 / kRate) l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 0.5 * std::sin(2.0 * dsp::kPi * phase);
        engine->process(io, io, 256);
        out.insert(out.end(), l.begin(), l.end());
    };
    for (int b = 0; b < 100; ++b) block();
    const size_t before = out.size();
    CHECK(engine->setProperty(static_cast<size_t>(k), file.path, true));
    CHECK(engine->fileStatus(static_cast<size_t>(k)).state == FileStatus::State::Ready);
    for (int b = 0; b < 20; ++b) block();
    // The largest step across the change is no larger than the room's own.
    double usual = 0.0, across = 0.0;
    for (size_t n = 1; n < out.size(); ++n) {
        const double step = std::fabs(out[n] - out[n - 1]);
        if (n + 512 < before) usual = std::max(usual, step);
        else if (n >= before && n < before + 512) across = std::max(across, step);
    }
    CHECK_MSG(across <= usual * 1.5 + 1e-9, "across %.4f, usual %.4f", across, usual);
    // And the tail follows the room: the file's is short.
    CHECK(engine->tail() < static_cast<uint32_t>(Processor::kRoomSeconds * kRate));
}

SUPPLE_TEST_MAIN
