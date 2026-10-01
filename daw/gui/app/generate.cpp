#include "generate.h"

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "supple/audio/audio_file.h"
#include "supple/base/files.h"

namespace supple::gui {
namespace {

// 30 s of stereo: a decaying pluck every beat at 120 BPM over noise-free
// partials, different per variant so the waveforms are told apart.
audio::AudioData variant(int k) {
    audio::AudioData a;
    a.sampleRate = 48000;
    const std::size_t frames = 30 * 48000;
    a.channels.assign(2, std::vector<float>(frames));
    const double f0 = 55.0 * std::pow(2.0, k / 4.0);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / a.sampleRate;
        const double beat = std::fmod(t, 0.5);
        const double env = std::exp(-beat * (4.0 + k)) * (0.35 + 0.25 * std::sin(t * 0.7 + k));
        const double s = std::sin(2 * std::numbers::pi * f0 * t) +
                         0.4 * std::sin(2 * std::numbers::pi * f0 * 2.01 * t);
        a.channels[0][i] = static_cast<float>(env * s * 0.6);
        a.channels[1][i] = static_cast<float>(env * s * 0.6 * (0.6 + 0.4 * std::cos(t * 0.3 + k)));
    }
    return a;
}

std::string pattern(int track) {
    static const char* names[] = {"C", "D", "E", "F", "G", "A", "B"};
    std::string bar = "1:";
    for (int n = 0; n < 8; ++n) {
        const int step = (track * 3 + n * (n % 3 + 1)) % 14;
        bar += " " + std::string(names[step % 7]) + std::to_string(3 + step / 7) + ":e";
    }
    return "voice lead\n" + bar + "\n2-8 = 1\n";
}

}  // namespace

Status generateSession(cmd::Dispatcher& dispatcher, const std::filesystem::path& mediaDir,
                       int tracks) {
    std::error_code ec;
    std::filesystem::create_directories(mediaDir, ec);
    std::vector<std::filesystem::path> files;
    for (int k = 0; k < 8; ++k) {
        const auto path = mediaDir / ("generated-" + std::to_string(k) + ".wav");
        if (!std::filesystem::exists(path)) {
            auto wav = audio::encodeWav(variant(k));
            if (!wav) return std::move(wav).error();
            if (auto w = writeFileAtomic(path, std::span<const std::uint8_t>(*wav)); !w) return w;
        }
        files.push_back(path);
    }
    auto run = [&](std::string command, json::Value args) -> Result<json::Value> {
        return dispatcher.execute(
            {std::move(command), std::move(args), {cmd::Origin::Type::Internal, "generate", {}}});
    };
    for (int i = 0; i < tracks; ++i) {
        json::Value add = json::Value::object();
        add["name"] = "Track " + std::to_string(i + 1);
        auto track = run("track.add", std::move(add));
        if (!track) return std::move(track).error();
        const std::string id = (*track)["track"].asString();
        json::Value audioItem = json::Value::object();
        audioItem["track"] = id;
        audioItem["bar"] = 1 + (i % 4);
        audioItem["audio_file"] = files[static_cast<std::size_t>(i) % files.size()].string();
        if (auto r = run("item.add", std::move(audioItem)); !r) return std::move(r).error();
        json::Value midiItem = json::Value::object();
        midiItem["track"] = id;
        midiItem["bar"] = 21;
        midiItem["notation"] = pattern(i);
        if (auto r = run("item.add", std::move(midiItem)); !r) return std::move(r).error();
    }
    return {};
}

}  // namespace supple::gui
