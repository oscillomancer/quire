#include <doctest/doctest.h>
#include <cmath>
#include <cstring>
#include <random>
#include "analyzer/processor.h"

using namespace supple;
using namespace supple::analyzer;
TEST_CASE("analyzer table and transactional canonical patch") {
    REQUIRE(params::validateTable(table()));
    for (std::size_t i = 0; i < table().size(); ++i) CHECK(table()[i].id == i);
    const auto& type = patchType();
    for (bool changed : {false, true}) {
        auto p = type.defaults();
        if (changed) {
            p.body.values[kWindow] = 3;
            p.body.values[kHold] = 1;
            p.body.values[kReset] = 1;
        }
        auto text = type.serialize(p);
        auto round = type.parse(text);
        REQUIRE(round);
        CHECK(type.serialize(*round) == text);
        auto bad = json::parse(R"({"params":{"window":"wrong","hold":27}})");
        REQUIRE(bad);
        const auto before = type.serialize(p);
        auto merged = type.merge(p, *bad);
        REQUIRE_FALSE(merged);
        CHECK(type.serialize(p) == before);
        REQUIRE_FALSE(merged.error().details.empty());
        CHECK(merged.error().details[0].path.find("/params/") == 0);
    }
    CHECK(describe(defaults()).size() <= 300);
}
TEST_CASE("analyzer exact pass-through across precision blocks rates and automation") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> noise(-.9, .9);
    for (double fs : {22050., 44100., 48000., 88200., 96000., 192000., 384000.}) {
        std::vector<double> original(8192), right(8192), l(8192), r(8192);
        for (std::size_t i = 0; i < original.size(); ++i) {
            original[i] = noise(rng);
            right[i] = noise(rng);
        }
        for (int block : {1, 7, 64, 4096}) {
            Processor p;
            p.prepare(fs, 4096, 2);
            l = original;
            r = right;
            for (int pos = 0; pos < 8192;) {
                if (pos % 37 == 0) {
                    p.setParam(kHold, (pos / 37) % 2);
                    p.setParam(kWindow, (pos / 37) % 4);
                    p.setParam(kReset, 1);
                }
                const int n = std::min({block, 8192 - pos, 37 - pos % 37});
                double* io[]{l.data() + pos, r.data() + pos};
                p.process(io, n);
                p.drain();
                pos += n;
            }
            CHECK(std::memcmp(l.data(), original.data(), l.size() * sizeof(double)) == 0);
            CHECK(std::memcmp(r.data(), right.data(), r.size() * sizeof(double)) == 0);
            CHECK(p.latency() == 0);
            std::vector<float> f(original.begin(), original.end()), copy = f;
            p.prepare(fs, 4096, 1);
            float* io[]{f.data()};
            p.process(io, static_cast<int>(f.size()));
            CHECK(std::memcmp(f.data(), copy.data(), f.size() * sizeof(float)) == 0);
        }
    }
}
TEST_CASE("analyzer automation stress five seconds is finite and untouched") {
    Processor p;
    p.prepare(48000, 64, 2);
    std::mt19937 rng(16);
    std::uniform_real_distribution<float> noise(-.9f, .9f);
    std::array<float, 64> l{}, r{}, original{};
    for (int pos = 0; pos < 240000;) {
        const int count = std::min(1 + static_cast<int>(rng() % 64), 240000 - pos);
        for (std::uint32_t i = 0; i < kNumParams; ++i)
            p.setParam(i, table()[i].min + (table()[i].max - table()[i].min) *
                                               static_cast<double>(rng() % 1000) / 999);
        for (int i = 0; i < count; ++i)
            l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] = noise(rng);
        original = l;
        float* io[]{l.data(), r.data()};
        p.process(io, count);
        CHECK(l == original);
        p.drain();
        pos += count;
    }
}
TEST_CASE("analyzer hold reset window and overflow are measurement-only") {
    Processor p;
    p.prepare(48000, 256, 2);
    p.setParam(kWindow, 3);
    std::array<float, 256> l{}, r{};
    for (int packet = 0; packet < 800; ++packet) {
        for (int i = 0; i < 256; ++i)
            l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] =
                static_cast<float>(.1 * std::sin((packet * 256 + i) * .13));
        float* io[]{l.data(), r.data()};
        p.process(io, 256);
        p.drain();
    }
    Frame before;
    REQUIRE(p.readFrame(before));
    CHECK(before.measurements.integrated > -120);
    p.setParam(kHold, 1);
    p.drain();
    REQUIRE(p.readFrame(before));
    for (int i = 0; i < 40; ++i) {
        float* io[]{l.data(), r.data()};
        p.process(io, 256);
        p.drain();
    }
    Frame held;
    REQUIRE(p.readFrame(held));
    CHECK(held.held);
    CHECK(held.measurements.seconds == before.measurements.seconds);
    p.setParam(kReset, 1);
    p.drain();
    Frame reset;
    REQUIRE(p.readFrame(reset));
    CHECK(reset.measurements.integrated == -120);
    CHECK(reset.held);
    p.setParam(kHold, 0);
    p.setParam(kWindow, 1);
    p.drain();
    Frame window;
    REQUIRE(p.readFrame(window));
    CHECK(window.window == 1);
    CHECK(window.measurements.seconds == 0);
    for (int i = 0; i < 700; ++i) {
        float* io[]{l.data(), r.data()};
        p.process(io, 256);
    }
    CHECK(p.droppedFrames() > 0);
    p.drain();
    float* io[]{l.data(), r.data()};
    p.process(io, 256);
    p.drain();
    Frame gap;
    REQUIRE(p.readFrame(gap));
    CHECK(gap.droppedFrames > 0);
    CHECK(gap.measurements.seconds < .1);
    auto doc = telemetryJson(gap);
    auto text = json::writeCanonical(doc);
    auto parsed = json::parse(text);
    REQUIRE(parsed);
    CHECK(parsed->find("schema")->asString() == "supple.analysis-report/1.0");
    REQUIRE(parsed->find("source"));
    CHECK(parsed->find("source")->find("channels")->asDouble() == 2);
    REQUIRE(parsed->find("spectral"));
    CHECK(parsed->find("spectral")->find("ltas_db")->asArray().size() == 31);
}
TEST_CASE("analyzer state restoration preserves measurements and output") {
    auto patch = patchType().defaults();
    patch.body.values[kWindow] = 3;
    auto text = patchType().serialize(patch);
    auto restored = patchType().parse(text);
    REQUIRE(restored);
    Processor a, b;
    a.prepare(48000, 256);
    b.prepare(48000, 256);
    for (std::size_t i = 0; i < table().size(); ++i) {
        a.setParam(i, patch.body.values[i]);
        b.setParam(i, restored->body.values[i]);
    }
    std::array<float, 256> l{}, r{}, x{}, y{};
    for (int block = 0; block < 200; ++block) {
        for (int i = 0; i < 256; ++i)
            l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] =
                static_cast<float>(.1 * std::sin((block * 256 + i) * .017));
        x = l;
        y = r;
        float *ia[]{l.data(), r.data()}, *ib[]{x.data(), y.data()};
        a.process(ia, 256);
        b.process(ib, 256);
        a.drain();
        b.drain();
        CHECK(l == x);
        CHECK(r == y);
    }
    Frame fa, fb;
    REQUIRE(a.readFrame(fa));
    REQUIRE(b.readFrame(fb));
    CHECK(json::writeCanonical(telemetryJson(fa)) == json::writeCanonical(telemetryJson(fb)));
}

TEST_CASE("measurement grid is identical across host partitions with offset controls") {
    constexpr int frames = 96000;
    std::vector<float> l(frames), r(frames), a(frames), b(frames);
    for (int i = 0; i < frames; ++i) {
        l[static_cast<std::size_t>(i)] = static_cast<float>(.1 * std::sin(i * .13));
        r[static_cast<std::size_t>(i)] = static_cast<float>(.13 * std::sin(i * .17));
    }
    std::string reference;
    for (int block : {1, 7, 64, 4096}) {
        Processor p;
        p.prepare(48000, 4096);
        p.setParam(kWindow, 3);
        for (int pos = 0; pos < frames;) {
            if (pos == 1234) p.setParam(kHold, 1);
            if (pos == 4321) p.setParam(kHold, 0);
            if (pos == 12567) p.setParam(kReset, 1);
            int next = frames;
            for (int event : {1234, 4321, 12567})
                if (event > pos) next = std::min(next, event);
            const int count = std::min(block, next - pos);
            const float* in[]{l.data() + pos, r.data() + pos};
            float* out[]{a.data() + pos, b.data() + pos};
            p.process(in, out, static_cast<uint32_t>(count));
            p.drain();
            pos += count;
        }
        CHECK(a == l);
        CHECK(b == r);
        Frame frame;
        REQUIRE(p.readFrame(frame));
        auto text = json::writeCanonical(telemetryJson(frame));
        if (reference.empty()) reference = text;
        CHECK(text == reference);
    }
}

TEST_CASE("live telemetry validates the supported analysis report fields") {
    Frame frame;
    frame.measurements.spectrum.fill(-120);
    auto value = json::parse(json::writeCanonical(telemetryJson(frame)));
    REQUIRE(value);
    REQUIRE(value->find("schema"));
    CHECK(value->find("schema")->asString() == "supple.analysis-report/1.0");
    for (const char* group : {"source", "loudness", "spectral", "stereo", "live"}) {
        REQUIRE(value->find(group));
        CHECK(value->find(group)->isObject());
    }
    const auto& source = *value->find("source");
    for (const char* field : {"seconds", "sample_rate", "channels"}) {
        REQUIRE(source.find(field));
        CHECK(source.find(field)->isNumber());
    }
    CHECK(source.find("layout")->asString() == "stereo");
    const auto& loudness = *value->find("loudness");
    for (const char* field :
         {"integrated_lufs", "lra_lu", "max_momentary_lufs", "max_short_term_lufs",
          "true_peak_dbtp", "sample_peak_dbfs", "plr_lu"}) {
        REQUIRE(loudness.find(field));
        CHECK(loudness.find(field)->isNumber());
        CHECK(std::isfinite(loudness.find(field)->asDouble()));
    }
    for (const char* field : {"momentary_lufs", "short_term_lufs"}) {
        REQUIRE(loudness.find(field));
        REQUIRE(loudness.find(field)->isArray());
        REQUIRE(loudness.find(field)->size() == 1);
        CHECK(loudness.find(field)->at(0).isNumber());
    }
    const auto& spectral = *value->find("spectral");
    REQUIRE(spectral.find("ltas_db"));
    REQUIRE(spectral.find("ltas_db")->size() == 31);
    for (const auto& level : spectral.find("ltas_db")->asArray()) {
        CHECK(level.isNumber());
        CHECK(std::isfinite(level.asDouble()));
    }
    REQUIRE(spectral.find("centroid_hz"));
    REQUIRE(spectral.find("centroid_hz")->isObject());
    CHECK(spectral.find("centroid_hz")->find("median")->isNumber());
    const auto& stereo = *value->find("stereo");
    for (const char* field : {"correlation", "width", "balance_db"}) {
        REQUIRE(stereo.find(field));
        CHECK(stereo.find(field)->isNumber());
        CHECK(std::isfinite(stereo.find(field)->asDouble()));
    }
}
