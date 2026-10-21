// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <supple/clap/plugin_support.h>
#include <supple/clap/semantic_model.h>

#include <supple/json/json.h>
#include <atomic>
#include <cstring>
#include <thread>

using namespace supple;

namespace {
constexpr params::ParamSpec kT[] = {
    {.id = 10,
     .key = "freq",
     .name = "Freq",
     .unit = units::Unit::Hz,
     .min = 20,
     .max = 20000,
     .def = 1000,
     .curve = {params::CurveKind::Log}},
    {.id = 11, .key = "on", .name = "On", .type = params::Type::Bool, .min = 0, .max = 1, .def = 1},
    {.id = 12,
     .key = "meter",
     .name = "Meter",
     .unit = units::Unit::dB,
     .min = -120,
     .max = 0,
     .def = -120,
     .flags = params::kReadonly},
};
}

TEST_CASE("ParamStore: physical values, CLAP domain, generations") {
    clapx::ParamStore s(kT);
    CHECK(s.get(0) == 1000);
    const auto g0 = s.generation();
    s.set(0, 50000);  // clamped
    CHECK(s.get(0) == 20000);
    CHECK(s.generation() > g0);
    double v = -1;
    REQUIRE(s.getClap(10, v));
    CHECK(v == 1.0);
    REQUIRE(s.setClap(10, 0.0));
    CHECK(s.get(0) == 20);
    CHECK_FALSE(s.setClap(12, 0.5));  // readonly
    CHECK_FALSE(s.setClap(99, 0.5));
    auto snap = s.snapshot();
    s.resetToDefaults();
    CHECK(s.get(0) == 1000);
    s.assign(snap);
    CHECK(s.get(0) == 20);
}

TEST_CASE("ClapParams: info and text follow core-params") {
    clap_param_info_t info{};
    REQUIRE(clapx::ClapParams::info(kT, 0, &info));
    CHECK(info.id == 10);
    CHECK(info.min_value == 0.0);
    CHECK(info.max_value == 1.0);
    CHECK(std::string(info.name) == "Freq");
    REQUIRE(clapx::ClapParams::info(kT, 2, &info));
    CHECK((info.flags & CLAP_PARAM_IS_READONLY) != 0);
    CHECK_FALSE(clapx::ClapParams::info(kT, 3, &info));
    char text[64];
    REQUIRE(clapx::ClapParams::valueToText(kT, 10, 1.0, text, sizeof(text)));
    CHECK(std::string(text) == "20.0 kHz");
    double v = 0;
    REQUIRE(clapx::ClapParams::textToValue(kT, 10, "20 Hz", &v));
    CHECK(v == 0.0);
    CHECK_FALSE(clapx::ClapParams::textToValue(kT, 10, "loud", &v));
    char tiny[4];
    REQUIRE(clapx::ClapParams::valueToText(kT, 10, 1.0, tiny, sizeof(tiny)));
    CHECK(std::string(tiny) == "20.");
}

TEST_CASE("ClapParams: unit-prefix boundary text is stable") {
    const params::ParamSpec time{.id = 20,
                                 .key = "delay.time",
                                 .name = "Delay Time",
                                 .unit = units::Unit::ms,
                                 .min = 1,
                                 .max = 2000,
                                 .def = 500};
    for (const auto& p : {kT[0], time}) {
        const double physical = p.unit == units::Unit::Hz ? 1000.6 : 999.6;
        char first[64], second[64];
        const double initial = params::toClap(p, physical);
        REQUIRE(clapx::ClapParams::valueToText({&p, 1}, p.id, initial, first, sizeof(first)));
        double parsed = 0;
        REQUIRE(clapx::ClapParams::textToValue({&p, 1}, p.id, first, &parsed));
        REQUIRE(clapx::ClapParams::valueToText({&p, 1}, p.id, parsed, second, sizeof(second)));
        CHECK(std::string(first) == second);
    }
}

TEST_CASE("errorsToJson flattens schema.invalid into its details") {
    Error e("schema.invalid", "2 problems");
    e.details.push_back(Error("a", "first", "/x", "fix x"));
    e.details.push_back(Error("b", "second", "/y"));
    auto arr = json::parse(clapx::errorsToJson(e));
    REQUIRE(arr);
    REQUIRE(arr->size() == 2);
    CHECK(arr->at(0).find("path")->asString() == "/x");
    auto one = json::parse(clapx::errorsToJson(Error("c", "only", "/z", "hint")));
    REQUIRE(one);
    CHECK(one->size() == 1);
}

TEST_CASE("ParamStore: readAll never observes half of an assign()" * doctest::description("rt")) {
    static constexpr params::ParamSpec kMany[] = {
        {.id = 0, .key = "a", .name = "A", .max = 1e6},
        {.id = 1, .key = "b", .name = "B", .max = 1e6},
        {.id = 2, .key = "c", .name = "C", .max = 1e6},
        {.id = 3, .key = "d", .name = "D", .max = 1e6},
        {.id = 4, .key = "e", .name = "E", .max = 1e6},
        {.id = 5, .key = "f", .name = "F", .max = 1e6},
    };
    clapx::ParamStore s(kMany);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        patch::FlatBody b;
        b.values.resize(6);
        for (int k = 1; k <= 20000; ++k) {
            for (auto& v : b.values) v = k;
            s.assign(b);
        }
        stop = true;
    });
    double v[6];
    std::uint64_t reads = 0, torn = 0;
    while (!stop.load()) {
        if (!s.readAll(v)) continue;
        ++reads;
        for (double x : v)
            if (x != v[0]) ++torn;
    }
    writer.join();
    CHECK(torn == 0);
    REQUIRE(s.readAll(v));
    CHECK(v[5] == 20000.0);
    MESSAGE("consistent reads during writes: ", reads);
}

TEST_CASE("semantic helpers reject invalid stream counts, callbacks and null patch text") {
    struct Model : clapx::SemanticModel {
        params::Table table() const override { return kT; }
        std::string patchTypeId() const override { return "supple.stream-test-patch/1.0"; }
        json::Value patchSchema() const override { return json::Value::object(); }
        std::string patchJson(bool) const override { return "{}"; }
        Status apply(std::uint32_t, const json::Value&) override { return okStatus(); }
        std::string describe(bool) const override { return "test"; }
    } model;
    clapx::SemanticAdapter adapter(model, {.id = "com.suppleaudio.test.stream",
                                           .name = "Stream",
                                           .version = "1",
                                           .docs = "",
                                           .stateMagic = 0x12345678});
    clap_istream_t in{nullptr, [](const clap_istream_t*, void*, std::uint64_t size) {
                          return static_cast<std::int64_t>(size + 1);
                      }};
    clap_ostream_t out{nullptr, [](const clap_ostream_t*, const void*, std::uint64_t size) {
                           return static_cast<std::int64_t>(size + 1);
                       }};
    CHECK_FALSE(adapter.loadState(&in));
    CHECK_FALSE(adapter.saveState(&out));
    CHECK_FALSE(adapter.getSchema(&out));
    CHECK_FALSE(adapter.loadState(nullptr));
    CHECK_FALSE(adapter.saveState(nullptr));
    in.read = nullptr;
    out.write = nullptr;
    CHECK_FALSE(adapter.loadState(&in));
    CHECK_FALSE(adapter.getSchema(&out));
    CHECK_FALSE(adapter.setPatch(CLAP_SUPPLE_SET_REPLACE, nullptr, 10, nullptr));
}
TEST_CASE("parameter events reject missing or undersized records") {
    clapx::ParamStore store(kT);
    clap_event_header_t header{sizeof(clap_event_header_t), 0, CLAP_CORE_EVENT_SPACE_ID,
                               CLAP_EVENT_PARAM_VALUE, 0};
    clap_input_events_t input{
        &header, [](const clap_input_events_t*) -> std::uint32_t { return 2; },
        [](const clap_input_events_t* list, std::uint32_t index) -> const clap_event_header_t* {
            return index == 0 ? static_cast<const clap_event_header_t*>(list->ctx) : nullptr;
        }};
    CHECK_FALSE(clapx::ClapParams::applyEvents(store, &input));
    CHECK(store.get(0) == 1000);
}
