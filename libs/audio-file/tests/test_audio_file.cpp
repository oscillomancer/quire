#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <limits>

#include <supple/audio/audio_file.h>

using namespace supple::audio;
namespace fs = std::filesystem;

namespace {
AudioData tone(std::size_t frames, double rate, unsigned channels) {
    AudioData d;
    d.sampleRate = rate;
    d.channels.assign(channels, std::vector<float>(frames));
    for (unsigned c = 0; c < channels; ++c)
        for (std::size_t i = 0; i < frames; ++i)
            d.channels[c][i] = static_cast<float>(
                0.5 * std::sin(2 * M_PI * 440.0 * static_cast<double>(i) / rate + c));
    return d;
}
const fs::path kData = fs::path(__FILE__).parent_path() / "data";
}  // namespace

TEST_CASE("float32 WAV round trip is bit-exact") {
    auto d = tone(1000, 48000, 2);
    auto bytes = encodeWav(d);
    REQUIRE(bytes);
    CHECK(detect(*bytes) == Container::Wav);
    auto back = readAudio(*bytes);
    REQUIRE(back);
    CHECK(back->sampleRate == 48000);
    REQUIRE(back->channelCount() == 2);
    REQUIRE(back->frames() == 1000);
    CHECK(back->channels == d.channels);
}

TEST_CASE("PCM WAV round trips within quantization + dither, deterministic by seed") {
    auto d = tone(4800, 44100, 1);
    for (auto fmt : {SampleFormat::Pcm16, SampleFormat::Pcm24}) {
        const double lsb = fmt == SampleFormat::Pcm16 ? 1.0 / 32768 : 1.0 / 8388608;
        auto a = encodeWav(d, {fmt, true, 7});
        auto b = encodeWav(d, {fmt, true, 7});
        auto c = encodeWav(d, {fmt, true, 8});
        REQUIRE(a);
        CHECK(*a == *b);
        CHECK(*a != *c);
        auto back = readAudio(*a);
        REQUIRE(back);
        double maxErr = 0;
        for (std::size_t i = 0; i < d.frames(); ++i)
            maxErr = std::max(
                maxErr, std::fabs(static_cast<double>(back->channels[0][i] - d.channels[0][i])));
        CHECK(maxErr <= 1.5 * lsb + 1e-9);
    }
    // Without dither, quantization error is at most half an LSB.
    auto nd = readAudio(*encodeWav(d, {SampleFormat::Pcm16, false, 0}));
    REQUIRE(nd);
    double maxErr = 0;
    for (std::size_t i = 0; i < d.frames(); ++i)
        maxErr =
            std::max(maxErr, std::fabs(static_cast<double>(nd->channels[0][i] - d.channels[0][i])));
    CHECK(maxErr <= 0.5 / 32768 + 1e-9);
}

TEST_CASE("clipping at full scale, invalid inputs rejected") {
    AudioData d;
    d.sampleRate = 48000;
    d.channels = {{2.0f, -2.0f, 1.0f}};
    auto back = readAudio(*encodeWav(d, {SampleFormat::Pcm16, false, 0}));
    REQUIRE(back);
    CHECK(static_cast<double>(back->channels[0][0]) == doctest::Approx(32767.0 / 32768));
    CHECK(static_cast<double>(back->channels[0][1]) == -1.0);
    AudioData empty;
    CHECK(encodeWav(empty).error().code == "audio.encode");
    AudioData ragged;
    ragged.channels = {{0.f, 0.f}, {0.f}};
    CHECK_FALSE(encodeWav(ragged));
    std::vector<std::uint8_t> junk = {1, 2, 3, 4, 5};
    CHECK(readAudio(junk).error().code == "audio.format");
    CHECK(readAudio(fs::path("/nonexistent/x.wav")).error().code == "audio.open");
}

TEST_CASE("FLAC and MP3 fixtures decode") {
    auto flac = readAudio(kData / "sine440.flac");
    REQUIRE(flac);
    CHECK(flac->sampleRate == 44100);
    CHECK(flac->channelCount() == 2);
    CHECK(flac->frames() == 11025);
    float peak = 0;
    for (float s : flac->channels[0]) peak = std::max(peak, std::fabs(s));
    CHECK(static_cast<double>(peak) == doctest::Approx(0.5).epsilon(0.01));
    auto mp3 = readAudio(kData / "sine440.mp3");
    REQUIRE(mp3);
    CHECK(mp3->sampleRate == 44100);
    CHECK(mp3->channelCount() == 2);
    CHECK(mp3->frames() > 10000);
}

TEST_CASE("writeWav is atomic and creates directories") {
    const fs::path dir = fs::temp_directory_path() / "supple-audio-file-test";
    fs::remove_all(dir);
    const fs::path file = dir / "sub" / "out.wav";
    REQUIRE(writeWav(file, tone(100, 48000, 2)).ok());
    CHECK(fs::exists(file));
    CHECK_FALSE(fs::exists(file.string() + ".tmp"));
    auto back = readAudio(file);
    REQUIRE(back);
    CHECK(back->frames() == 100);
    fs::remove_all(dir);
}

TEST_CASE("truncated WAV data decodes what is there and is flagged") {
    AudioData a;
    a.sampleRate = 48000;
    a.channels.assign(2, std::vector<float>(1000, 0.25f));
    WavOptions o;
    o.format = SampleFormat::Pcm16;
    auto bytes = encodeWav(a, o).value();
    CHECK_FALSE(readAudio(bytes).value().truncated);
    bytes.resize(bytes.size() - 100);
    auto r = readAudio(bytes);
    REQUIRE(r);
    CHECK(r->truncated);
    CHECK(r->frames() == 975);
}

TEST_CASE("PCM encoding maps NaN to zero and clamps infinities") {
    AudioData a;
    a.sampleRate = 48000;
    a.channels = {{std::nanf(""), std::numeric_limits<float>::infinity(),
                   -std::numeric_limits<float>::infinity(), 0.5f}};
    for (auto f : {SampleFormat::Pcm16, SampleFormat::Pcm24}) {
        WavOptions o;
        o.format = f;
        o.dither = false;
        auto back = readAudio(encodeWav(a, o).value());
        REQUIRE(back);
        const auto& c = back->channels[0];
        CHECK(c[0] == 0.0f);
        CHECK(c[1] == doctest::Approx(1.0).epsilon(1e-4));
        CHECK(c[2] == -1.0f);
        CHECK(c[3] == 0.5f);
    }
}

TEST_CASE("resampling preserves channel layout and duration") {
    auto input = tone(4800, 48000, 2);
    auto output = resample(input, 44100);
    REQUIRE(output);
    CHECK(output->sampleRate == 44100);
    CHECK(output->channelCount() == 2);
    CHECK(output->frames() == 4410);
    input.truncated = true;
    CHECK(resample(input, 44100)->truncated);
    CHECK(std::abs(output->seconds() - input.seconds()) < 1e-12);
    CHECK_FALSE(resample(input, -1));
}
