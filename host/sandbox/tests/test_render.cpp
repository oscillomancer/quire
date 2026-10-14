#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <supple/clap/semantic_params.h>
#include <supple/params/params.h>
#include <supple/sandbox/render.h>

using namespace supple;
using namespace supple::sandbox;

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;

std::unique_ptr<host::Instance> make(const std::string& id) {
    auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
    REQUIRE(list);
    for (const auto& p : *list)
        if (p.id == id) {
            auto inst = host::Instance::create(p, {});
            REQUIRE(inst);
            return std::move(*inst);
        }
    FAIL("fixture missing: ", id);
    return nullptr;
}

TimedEvent note(std::int64_t at, bool on, int key = 69) {
    TimedEvent e;
    e.sample = at;
    e.event.kind = on ? host::Event::Kind::NoteOn : host::Event::Kind::NoteOff;
    e.event.key = static_cast<std::int16_t>(key);
    e.event.velocity = 1.0;
    return e;
}

double energy(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double e = 0;
    for (std::size_t i = from; i < to && i < v.size(); ++i)
        e += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    return e;
}

}  // namespace

TEST_CASE("instrument render: notes at exact samples, silent tail detection" *
          doctest::description("rt")) {
    auto sine = make("com.suppleaudio.test.sine");
    host::Instance* chain[] = {sine.get()};
    RenderSettings s;
    RenderRequest req;
    req.events = {note(1000, true), note(5000, false)};
    auto r = render(chain, s, req);
    REQUIRE(r);
    const auto& L = r->audio.channels[0];
    CHECK(energy(L, 0, 1001) == 0.0);  // sine starts at phase 0 at sample 1000
    CHECK(energy(L, 1001, 5000) > 10.0);
    CHECK(energy(L, 5000, L.size()) == 0.0);
    CHECK(r->latency == 0);
    CHECK_FALSE(r->tailCapped);
    // Musical length 5001 + a 0.2 s silent hold, rounded to whole blocks.
    CHECK(L.size() >= 5001 + 9600);
    CHECK(L.size() < 5001 + 9600 + 2 * 256);
    CHECK(r->peakDbfs == doctest::Approx(20 * std::log10(0.25)).epsilon(0.001));
}

TEST_CASE("renders are deterministic and block-size invariant") {
    auto sine = make("com.suppleaudio.test.sine");
    host::Instance* chain[] = {sine.get()};
    RenderRequest req;
    req.events = {note(10, true, 60), note(333, true, 64), note(4000, false, 60),
                  note(4500, false, 64)};
    req.lengthSamples = 6000;
    RenderSettings s;
    s.tailLimitSeconds = 0.0;  // fixed length
    auto a = render(chain, s, req);
    auto b = render(chain, s, req);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->audio.channels == b->audio.channels);
    for (std::uint32_t bs : {1u, 64u, 1000u, 4096u}) {
        s.blockSize = bs;
        auto c = render(chain, s, req);
        REQUIRE(c);
        CHECK(c->audio.channels == a->audio.channels);
    }
}

TEST_CASE("chain latency is compensated") {
    auto sine = make("com.suppleaudio.test.sine");
    auto gain = make("com.suppleaudio.test.gain");
    RenderRequest req;
    req.events = {note(100, true), note(2000, false)};
    req.lengthSamples = 3000;
    RenderSettings s;
    s.tailLimitSeconds = 0.0;
    host::Instance* dry[] = {sine.get()};
    auto reference = render(dry, s, req);
    REQUIRE(reference);
    REQUIRE(gain->setPatch(R"({"params": {"latency": "short"}})", CLAP_SUPPLE_SET_MERGE).ok());
    host::Instance* chain[] = {sine.get(), gain.get()};
    auto r = render(chain, s, req);
    REQUIRE(r);
    CHECK(r->latency == 64);
    REQUIRE(r->audio.frames() == reference->audio.frames());
    CHECK(r->audio.channels == reference->audio.channels);  // 0 dB gain, aligned
}

TEST_CASE("effect render with input audio and a targeted parameter event") {
    auto gain = make("com.suppleaudio.test.gain");
    audio::AudioData in;
    in.sampleRate = 48000;
    in.channels.assign(2, std::vector<float>(4000, 0.5f));
    RenderRequest req;
    req.input = &in;
    TimedEvent change;
    change.sample = 1003;
    change.target = 0;
    change.event.kind = host::Event::Kind::Param;
    change.event.paramId = 0;
    auto schema = gain->semanticSchema();
    REQUIRE(schema);
    auto param = params::fromDescription("gain", (*schema)["params"]["gain"]);
    REQUIRE(param);
    change.event.value = params::toClap((*param)->spec, -6.0);  // physical dB, not a host value
    auto mute = change;
    mute.sample = 2000;
    mute.event.value = params::toClap((*param)->spec, (*param)->spec.min);
    req.events = {change, mute};
    RenderSettings s;
    s.tailLimitSeconds = 0.0;
    host::Instance* chain[] = {gain.get()};
    for (std::uint32_t block : {256u, 1000u}) {
        REQUIRE(gain->setPatch(R"({"params":{"gain":0}})", CLAP_SUPPLE_SET_MERGE));
        s.blockSize = block;
        auto r = render(chain, s, req);
        REQUIRE(r);
        REQUIRE(r->audio.frames() == 4000);
        CHECK(static_cast<double>(r->audio.channels[0][1002]) == doctest::Approx(0.5));
        CHECK(static_cast<double>(r->audio.channels[0][1003]) ==
              doctest::Approx(0.5 * std::pow(10.0, -6.0 / 20.0)));
        CHECK(static_cast<double>(r->audio.channels[0][1999]) ==
              doctest::Approx(0.5 * std::pow(10.0, -6.0 / 20.0)));
        CHECK(r->audio.channels[0][2000] == 0.0f);
    }
    // Wrong input rate is rejected.
    in.sampleRate = 44100;
    auto bad = render(chain, s, req);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "render.input_rate");
}

TEST_CASE("renderer mixes a mono key at exact samples and zero pads it" *
          doctest::description("rt")) {
    auto mix = make("com.suppleaudio.test.keymix");
    host::Instance* chain[] = {mix.get()};
    audio::AudioData input, key;
    input.sampleRate = key.sampleRate = 48000;
    input.channels.assign(2, std::vector<float>(3000, 0.25f));
    key.channels.assign(1, std::vector<float>(2013, 0.0f));
    std::fill(key.channels[0].begin() + 1003, key.channels[0].end(), 0.5f);
    RenderRequest request;
    request.input = &input;
    request.sidechain = &key;
    RenderSettings settings;
    settings.tailLimitSeconds = 0;
    for (std::uint32_t block : {256u, 1000u}) {
        settings.blockSize = block;
        auto r = render(chain, settings, request);
        REQUIRE(r);
        REQUIRE(r->audio.frames() == 3000);
        std::size_t wrong = 0;
        for (const auto& channel : r->audio.channels)
            for (std::size_t i = 0; i < channel.size(); ++i)
                wrong += channel[i] != (i >= 1003 && i < 2013 ? 0.75f : 0.25f);
        CHECK(wrong == 0);
    }
    // No key: the port is deactivated, and the fixture keys from its input.
    request.sidechain = nullptr;
    auto dry = render(chain, settings, request);
    REQUIRE(dry);
    for (const auto& channel : dry->audio.channels)
        CHECK(std::all_of(channel.begin(), channel.end(), [](float x) { return x == 0.5f; }));
    request.sidechain = &key;
    key.sampleRate = 44100;
    CHECK(render(chain, settings, request).error().path == "/sidechain");
    key.sampleRate = 48000;
    key.channels.push_back(std::vector<float>(1));
    CHECK(render(chain, settings, request).error().path == "/sidechain");
    key.channels.clear();
    CHECK(render(chain, settings, request).error().path == "/sidechain");
}

#if defined(__SSE2__)
TEST_CASE("a chain renders with FTZ and DAZ, as in a session (ADR-0025)" *
          doctest::description("rt")) {
    auto edge = make("com.suppleaudio.test.edge");
    const std::uint8_t probe[4] = {50, 0, 0, 0};  // the underflow probe
    REQUIRE(edge->loadState(probe).ok());
    host::Instance* chain[] = {edge.get()};
    audio::AudioData in;
    in.sampleRate = 48000;
    in.channels.assign(2, std::vector<float>(400));
    for (std::size_t i = 0; i < 400; ++i)
        for (auto& c : in.channels) c[i] = i % 4 == 0 ? 1e-7f : i % 4 == 1 ? 1e-9f : -1e-40f;
    RenderRequest req;
    req.input = &in;
    RenderSettings s;
    s.tailLimitSeconds = 0.0;
    auto r = render(chain, s, req);
    REQUIRE(r);
    REQUIRE(r->audio.frames() == 400);
    std::size_t wrong = 0;  // exact above 2^-26, flushed below
    for (const auto& c : r->audio.channels)
        for (std::size_t i = 0; i < 400; ++i) wrong += c[i] != (i % 4 == 0 ? 1e-7f : 0.0f);
    CHECK(wrong == 0);
}
#endif

TEST_CASE("layout and settings validation") {
    auto sine = make("com.suppleaudio.test.sine");
    auto gain = make("com.suppleaudio.test.gain");
    RenderSettings s;
    RenderRequest req;
    host::Instance* wrongOrder[] = {gain.get(), sine.get()};
    auto e = render(wrongOrder, s, req);
    REQUIRE_FALSE(e);
    CHECK(e.error().code == "render.layout");
    CHECK(render(std::span<host::Instance* const>{}, s, req).error().code == "render.empty_chain");
    s.blockSize = 0;
    host::Instance* chain[] = {sine.get()};
    CHECK(render(chain, s, req).error().code == "render.block_size");
}

TEST_CASE("transport positions") {
    RenderSettings s;
    s.tempo = 120;
    auto t = transportAt(s, 48000 * 3);  // 3 s at 120 BPM = 6 beats
    CHECK(t.beat == doctest::Approx(6.0));
    CHECK(t.bar == 1);
    CHECK(t.barStartBeat == doctest::Approx(4.0));
    s.numerator = 6;
    s.denominator = 8;  // 3 quarter-note beats per bar
    auto u = transportAt(s, 48000 * 3);
    CHECK(u.bar == 2);
    CHECK(u.barStartBeat == doctest::Approx(6.0));
}

TEST_CASE("render follows a tail that changes while the chain processes") {
    auto edge = make("com.suppleaudio.test.edge");
    const std::uint8_t learned[4] = {20, 0xe8, 0x03, 0};  // 1000, then 2000 once processing
    REQUIRE(edge->loadState(learned).ok());
    host::Instance* chain[] = {edge.get()};
    audio::AudioData in;
    in.sampleRate = 48000;
    in.channels.assign(2, std::vector<float>(4800, 0.0f));
    RenderRequest req;
    req.input = &in;
    RenderSettings s;
    for (int pass = 0; pass < 2; ++pass) {  // the same render twice: the same length
        auto r = render(chain, s, req);
        REQUIRE(r);
        CHECK(r->audio.frames() == 4800 + 2000);
        CHECK_FALSE(r->tailCapped);
    }
}

TEST_CASE("render ends when the chain's reported tail elapses") {
    auto edge = make("com.suppleaudio.test.edge");
    const std::uint8_t tail1000[4] = {0, 0xe8, 0x03, 0};
    REQUIRE(edge->loadState(tail1000).ok());
    host::Instance* chain[] = {edge.get()};
    audio::AudioData in;
    in.sampleRate = 48000;
    in.channels.assign(
        2, std::vector<float>(4800, 0.0f));  // silent: silence detection would wait 0.2 s
    RenderRequest req;
    req.input = &in;
    RenderSettings s;
    auto r = render(chain, s, req);
    REQUIRE(r);
    CHECK(r->audio.frames() == 4800 + 1000);  // 5800 is not a block multiple: exact stop
    CHECK(r->tailSamples == 1000);
    CHECK_FALSE(r->tailCapped);
}

TEST_CASE("render reports a refused start_processing") {
    auto edge = make("com.suppleaudio.test.edge");
    const std::uint8_t refuse[4] = {1, 0, 0, 0};
    REQUIRE(edge->loadState(refuse).ok());
    host::Instance* chain[] = {edge.get()};
    RenderRequest req;
    req.lengthSamples = 100;
    auto r = render(chain, RenderSettings{}, req);
    REQUIRE(!r);
    CHECK(r.error().code == "render.start_failed");
}

TEST_CASE("render validates sizes before allocating") {
    auto sine = make("com.suppleaudio.test.sine");
    host::Instance* chain[] = {sine.get()};
    RenderRequest req;
    req.lengthSamples = INT64_MAX;
    CHECK(render(chain, RenderSettings{}, req).error().code == "render.too_long");
    req.lengthSamples = -1;
    CHECK(render(chain, RenderSettings{}, req).error().code == "render.too_long");
    req.lengthSamples = 100;
    req.events = {note(-5, true)};
    CHECK(render(chain, RenderSettings{}, req).error().code == "render.event_time");
    req.events = {note(INT64_MAX - 1, true)};
    CHECK(render(chain, RenderSettings{}, req).error().code == "render.event_time");
    req.events.clear();
    RenderSettings nan;
    nan.tailLimitSeconds = std::nan("");
    CHECK(render(chain, nan, req).error().code == "render.tail");
    RenderSettings hold;
    hold.silenceHoldSeconds = 0;
    CHECK(render(chain, hold, req).error().code == "render.silence");
    req.lengthSamples = (std::int64_t(1) << 25) - 10;  // fits, but not with a 5 s tail
    CHECK(render(chain, RenderSettings{}, req).error().code == "render.too_long");
}

TEST_CASE("render rejects malformed transport, chain, input and nonfinite output") {
    auto edge = make("com.suppleaudio.test.edge");
    host::Instance* chain[] = {edge.get()};
    RenderSettings settings;
    settings.tailLimitSeconds = 0;
    RenderRequest request;
    request.lengthSamples = 64;
    for (double tempo : {0.0, std::numeric_limits<double>::quiet_NaN()}) {
        settings.tempo = tempo;
        CHECK_FALSE(render(chain, settings, request));
    }
    settings.tempo = 120;
    settings.denominator = 0;
    CHECK_FALSE(render(chain, settings, request));
    settings.denominator = 4;
    host::Instance* invalid[] = {nullptr};
    CHECK_FALSE(render(invalid, settings, request));
    host::Instance* duplicate[] = {edge.get(), edge.get()};
    CHECK_FALSE(render(duplicate, settings, request));
    audio::AudioData input;
    input.sampleRate = settings.sampleRate;
    input.channels = {std::vector<float>(64), std::vector<float>(32)};
    request.input = &input;
    CHECK_FALSE(render(chain, settings, request));
    input.channels[1].resize(64);
    input.channels[0][0] = std::numeric_limits<float>::quiet_NaN();
    auto result = render(chain, settings, request);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "render.process_error");
    input.channels[0][0] = 0;
    CHECK(render(chain, settings, request));
    REQUIRE(edge->startProcessing());
    CHECK_FALSE(render(chain, settings, request));
    edge->stopProcessing();
}
