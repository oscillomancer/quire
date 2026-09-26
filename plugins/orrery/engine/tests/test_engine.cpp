#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../src/oscillators.h"
#include "../src/oversampling.h"
#include "orrery/engine.h"

using namespace supple;
using namespace supple::orrery;

namespace {

struct Audio {
    std::vector<float> l, r;
    double peak() const {
        double p = 0.0;
        for (std::size_t i = 0; i < l.size(); ++i)
            p = std::max(
                {p, std::fabs(static_cast<double>(l[i])), std::fabs(static_cast<double>(r[i]))});
        return p;
    }
    double rms(std::size_t from, std::size_t to) const {
        double s = 0.0;
        for (std::size_t i = from; i < to && i < l.size(); ++i)
            s += static_cast<double>(l[i]) * static_cast<double>(l[i]);
        return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, to - from)));
    }
    bool finite() const {
        for (std::size_t i = 0; i < l.size(); ++i)
            if (!std::isfinite(l[i]) || !std::isfinite(r[i])) return false;
        return true;
    }
};

// Events with absolute sample times.
struct Timed {
    std::int64_t time;
    Event event;
};

Timed note(std::int64_t t, int key, double velocity) {
    Event e;
    e.type = Event::Type::NoteOn;
    e.key = static_cast<std::int16_t>(key);
    e.value = velocity;
    return {t, e};
}
Timed noteOff(std::int64_t t, int key) {
    Event e;
    e.type = Event::Type::NoteOff;
    e.key = static_cast<std::int16_t>(key);
    return {t, e};
}
Timed param(std::int64_t t, std::string_view key, double value) {
    Event e;
    e.type = Event::Type::Param;
    e.index =
        static_cast<std::uint32_t>(params::indexOf(table(), *params::findByKey(table(), key)));
    e.value = value;
    return {t, e};
}
Timed cc(std::int64_t t, int number, double value) {
    Event e;
    e.type = Event::Type::Controller;
    e.index = static_cast<std::uint32_t>(number);
    e.value = value;
    return {t, e};
}
Timed bend(std::int64_t t, double value) {
    Event e;
    e.type = Event::Type::Pitchbend;
    e.value = value;
    return {t, e};
}

Patch parse(std::string_view body) {
    static const PatchType type;
    auto p = type.parse(std::string(R"({"schema": "supple.orrery-patch/1.0", )") +
                        std::string(body) + "}");
    REQUIRE_MESSAGE(p, (p ? "" : p.error().message + " " + p.error().path));
    return *p;
}

Audio render(const Patch& patch, const std::vector<Timed>& events, std::int64_t frames,
             std::uint32_t block, double sr = 48000.0) {
    Engine engine;
    engine.prepare(sr, std::max<std::uint32_t>(block, 16));
    engine.setPatch(patch);
    engine.reset();
    Audio out;
    out.l.resize(static_cast<std::size_t>(frames));
    out.r.resize(static_cast<std::size_t>(frames));
    std::vector<Event> buf;
    std::size_t next = 0;
    for (std::int64_t pos = 0; pos < frames; pos += block) {
        const auto n = static_cast<std::uint32_t>(std::min<std::int64_t>(block, frames - pos));
        buf.clear();
        while (next < events.size() && events[next].time < pos + n) {
            Event e = events[next].event;
            e.time = static_cast<std::uint32_t>(std::max<std::int64_t>(0, events[next].time - pos));
            buf.push_back(e);
            ++next;
        }
        engine.process(buf, Transport{}, out.l.data() + pos, out.r.data() + pos, n);
    }
    return out;
}

constexpr std::string_view kPad = R"(
  "seed": 1234,
  "voice": {"unison": 3, "unison_detune": "12 ct", "unison_spread": "70 %", "drift": "10 %"},
  "oscillators": {
    "osc1": {"type": "va", "wave": "saw", "fine": "-7 ct"},
    "osc2": {"type": "va", "wave": "saw", "fine": "+7 ct"},
    "osc3": {"type": "sub", "level": "-12 dB"}
  },
  "filters": {"filter1": {"type": "ladder", "cutoff": "1.2 kHz", "resonance": "25 %", "keytrack": "50 %"}},
  "drive": {"type": "tape", "amount": "3 dB"},
  "amp": {"level": "-3 dB", "velocity": "40 %"},
  "envelopes": {
    "amp": {"attack": "30 ms", "decay": "300 ms", "sustain": "80 %", "release": "200 ms"},
    "env2": {"attack": "50 ms", "decay": "400 ms", "sustain": "30 %", "release": "300 ms"}
  },
  "lfos": {"lfo1": {"wave": "sine", "rate": "3 Hz", "retrigger": "free"},
           "lfo2": {"wave": "triangle", "rate": "5 Hz"}},
  "steps": {"step1": {"values": [100, 0, -50, 25], "length": 4, "sync": "1/16"}},
  "routes": {
    "env2>filter1.cutoff": {"amount": "+2 oct"},
    "lfo1>osc1.fine": {"amount": "8 ct"},
    "lfo2>osc2.pitch": {"amount": "0.2 st"},
    "step1>filter1.resonance": {"amount": "20 %"},
    "macro1>filter1.cutoff": {"amount": "+1 oct"},
    "modwheel>amp.pan": {"amount": "30 %"},
    "velocity>filter1.cutoff": {"amount": "+1 oct"}
  },
  "macros": {"macro1": {"name": "Brightness", "value": "20 %"}},
  "fx": {"chain": ["chorus", "delay", "reverb"],
         "chorus": {"mix": "35 %"},
         "delay": {"sync": "off", "time": "120 ms", "mix": "20 %"},
         "reverb": {"decay": "1.5 s", "mix": "25 %"}}
)";

std::vector<Timed> performance() {
    return {note(100, 60, 0.8), note(1234, 64, 0.6), param(3000, "filter1.cutoff", 800.0),
            cc(4100, 1, 0.7),   bend(5000, 0.5),     param(6007, "macro1", 80.0),
            noteOff(9000, 60),  note(9001, 67, 1.0), param(9500, "fx.reverb.mix", 40.0),
            noteOff(14000, 64), noteOff(15000, 67),  bend(15001, 0.0)};
}

}  // namespace

TEST_CASE("a note makes sound and the voice ends after its release") {
    const Patch p = parse(R"("oscillators": {"osc1": {"type": "va"}},
                             "envelopes": {"amp": {"attack": "5 ms", "release": "50 ms"}})");
    const Audio a = render(p, {note(0, 69, 1.0), noteOff(4800, 69)}, 48000, 256);
    CHECK(a.finite());
    CHECK(a.rms(480, 4800) > 0.05);
    CHECK(a.rms(24000, 48000) < 1e-6);
    CHECK(a.peak() < 1.0);
}

TEST_CASE("renders do not depend on the block size") {
    const Patch p = parse(kPad);
    const auto events = performance();
    const std::int64_t frames = 24000;
    const Audio ref = render(p, events, frames, 4096);
    CHECK(ref.finite());
    CHECK(ref.rms(2000, 9000) > 0.01);
    for (std::uint32_t block : {1u, 7u, 16u, 64u, 333u, 1024u}) {
        CAPTURE(block);
        const Audio a = render(p, events, frames, block);
        double worst = 0.0;
        for (std::size_t i = 0; i < a.l.size(); ++i)
            worst = std::max({worst, std::fabs(static_cast<double>(a.l[i] - ref.l[i])),
                              std::fabs(static_cast<double>(a.r[i] - ref.r[i]))});
        CHECK(worst == 0.0);
    }
}

TEST_CASE("the seed decides every random choice") {
    const Patch p = parse(kPad);
    Patch other = p;
    other.meta.seed = 99;
    const auto events = performance();
    const Audio a = render(p, events, 12000, 512), b = render(p, events, 12000, 512),
                c = render(other, events, 12000, 512);
    CHECK(a.l == b.l);
    CHECK(a.l != c.l);
}

TEST_CASE("every module type renders finite, bounded audio") {
    const std::vector<std::string> oscs = {
        R"({"type": "va", "wave": "pulse", "width": 30, "shape": 40})",
        R"({"type": "va", "wave": "triangle"})",
        R"({"type": "wavetable", "position": 50})",
        R"({"type": "fm", "ratio": 2, "feedback": 40})",
        R"({"type": "noise", "color": "pink"})",
        R"({"type": "noise", "color": "brown"})",
        R"({"type": "noise", "color": "blue"})",
        R"({"type": "sub", "wave": "square", "octave": -2})"};
    const std::vector<std::string> filters = {
        R"({"type": "ladder", "resonance": 95, "slope": "12 dB"})",
        R"({"type": "svf", "mode": "bandpass", "resonance": 90})",
        R"({"type": "svf", "mode": "notch"})",
        R"({"type": "comb", "cutoff": 220, "feedback": -80})",
        R"({"type": "formant", "vowel": 2.5})"};
    const std::vector<std::string> drives = {"soft", "tape", "fold", "hard"};
    for (std::size_t i = 0; i < oscs.size(); ++i) {
        const std::string body =
            R"("voice": {"quality": ")" +
            std::string(i % 3 == 0   ? "eco"
                        : i % 3 == 1 ? "normal"
                                     : "high") +
            R"("}, "oscillators": {"osc1": )" + oscs[i] + R"(}, "filters": {"filter1": )" +
            filters[i % filters.size()] + R"(}, "drive": {"type": ")" + drives[i % drives.size()] +
            R"(", "amount": 18}, "fx": {"chain": ["phaser", "eq"], "eq": {"low_gain": 6, "mid_gain": -6}})";
        CAPTURE(body);
        const Audio a = render(
            parse(body), {note(0, 45, 1.0), note(0, 57, 0.7), noteOff(9000, 45), noteOff(9000, 57)},
            14400, 128);
        CHECK(a.finite());
        CHECK(a.peak() <= 1.0);
        CHECK(a.rms(1000, 9000) > 1e-4);
    }
}

TEST_CASE("FM: a modulator-only operator changes the carrier's spectrum") {
    const Patch plain = parse(R"("oscillators": {"osc1": {"type": "fm"}})");
    const Patch fm = parse(
        R"("oscillators": {"osc1": {"type": "fm"}, "osc2": {"type": "fm", "ratio": 3, "level": 0, "to": "none"}},
                              "routes": {"osc2>osc1.fm": {"amount": 4}})");
    const Audio a = render(plain, {note(0, 57, 1.0)}, 9600, 256);
    const Audio b = render(fm, {note(0, 57, 1.0)}, 9600, 256);
    // Same level (the modulator is not heard), different waveform.
    CHECK(a.rms(2000, 9600) == doctest::Approx(b.rms(2000, 9600)).epsilon(0.25));
    double diff = 0.0;
    for (std::size_t i = 2000; i < 9600; ++i)
        diff += std::fabs(static_cast<double>(a.l[i] - b.l[i]));
    CHECK(diff > 100.0);
}

TEST_CASE("legato keeps the envelope running; mono retriggers") {
    auto levelAfterSecondNote = [](std::string_view mode, std::string_view retrigger) {
        const Patch p = parse(std::string(R"("voice": {"mode": ")") + std::string(mode) +
                              R"("}, "oscillators": {"osc1": {"type": "va", "wave": "sine"}},
                                 "envelopes": {"amp": {"attack": "5 ms", "decay": "40 ms", "sustain": 10,
                                                       "retrigger": ")" +
                              std::string(retrigger) + R"("}})");
        // The second key arrives while the first is held, after the decay.
        const Audio a = render(p, {note(0, 60, 1.0), note(4800, 62, 1.0)}, 7200, 64);
        return a.rms(4800 + 240, 4800 + 1200);
    };
    const double legato = levelAfterSecondNote("legato", "always");
    const double mono = levelAfterSecondNote("mono", "always");
    const double monoLegatoEnv = levelAfterSecondNote("mono", "legato");
    CHECK(mono > 2.0 * legato);
    CHECK(monoLegatoEnv == doctest::Approx(legato).epsilon(0.2));
}

TEST_CASE("sample rates and a program swap while playing") {
    const Patch p = parse(kPad);
    for (double sr : {22050.0, 44100.0, 96000.0, 192000.0}) {
        CAPTURE(sr);
        const Audio a =
            render(p, {note(0, 60, 0.8), noteOff(static_cast<std::int64_t>(sr * 0.2), 60)},
                   static_cast<std::int64_t>(sr * 0.4), 512, sr);
        CHECK(a.finite());
        CHECK(a.rms(static_cast<std::size_t>(sr * 0.05), static_cast<std::size_t>(sr * 0.2)) >
              0.01);
    }
    Engine engine;
    engine.prepare(48000.0, 256);
    engine.setPatch(p);
    engine.reset();
    std::vector<float> l(256), r(256);
    Event on;
    on.type = Event::Type::NoteOn;
    on.key = 60;
    on.value = 1.0;
    engine.process(std::span<const Event>(&on, 1), Transport{}, l.data(), r.data(), 256);
    for (int i = 0; i < 20; ++i) engine.process({}, Transport{}, l.data(), r.data(), 256);
    CHECK(engine.activeVoices() == 3);
    // Change values and structure mid-note: the voice keeps sounding.
    Patch q = parse(
        R"("voice": {"unison": 3}, "oscillators": {"osc1": {"type": "fm"}, "osc2": {"type": "va"}},
                       "filters": {"filter1": {"type": "svf", "cutoff": 500}})");
    engine.setPatch(q);
    double energy = 0.0;
    for (int i = 0; i < 20; ++i) {
        engine.process({}, Transport{}, l.data(), r.data(), 256);
        for (float x : l) energy += static_cast<double>(x) * static_cast<double>(x);
        engine.collect();
    }
    CHECK(engine.activeVoices() == 3);
    CHECK(energy > 1.0);
}

TEST_CASE("half-band decimator: coefficients meet the stopband target") {
    const auto c = designHalfband(6, 8.0 / 96.0);
    for (double f = 0.0; f <= 0.20; f += 0.005) CHECK(halfbandResponseDb(c, f) > -0.01);
    for (double f = 0.2917; f <= 0.5; f += 0.002) CHECK(halfbandResponseDb(c, f) < -90.0);
}

TEST_CASE("band-limited triangle is closer to the ideal than the naive one") {
    // Reference: additive odd harmonics below Nyquist.
    const double fs = 48000.0, hz = 3333.0, dt = hz / fs;
    double errBlamp = 0.0, errNaive = 0.0, phase = 0.1;
    for (int n = 0; n < 4800; ++n) {
        double ideal = 0.0;
        for (int k = 1; k * hz < fs / 2; k += 2)
            ideal +=
                (k % 4 == 1 ? 1.0 : -1.0) * std::sin(2.0 * M_PI * k * (phase - 0.25)) / (k * k);
        ideal *= -8.0 / (M_PI * M_PI);
        const double naive = phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase;
        const double bl = wave::triangle(phase, dt);
        errBlamp += (bl - ideal) * (bl - ideal);
        errNaive += (naive - ideal) * (naive - ideal);
        phase += dt;
        phase -= std::floor(phase);
    }
    CHECK(errBlamp < errNaive);
}

TEST_CASE("review round 3: steal bursts, all-sound-off, quality changes, AM via, held keys") {
    SUBCASE("a burst of steals never drops the newest note") {
        const Patch p = parse(R"("voice": {"polyphony": 32, "unison": 2},
                                 "oscillators": {"osc1": {"type": "va", "wave": "sine"}},
                                 "envelopes": {"amp": {"release": "5 ms"}})");
        std::vector<Timed> ev;
        for (int k = 0; k < 41; ++k) ev.push_back(note(0, 30 + k, 1.0));
        for (int k = 0; k < 40; ++k) ev.push_back(noteOff(10, 30 + k));
        const Audio a = render(p, ev, 24000, 256);
        CHECK(a.rms(12000, 24000) > 0.01);  // key 70 still sounds
    }
    SUBCASE("CC120 silences notes held by the sustain pedal; CC123 does not") {
        const Patch p = parse(R"("oscillators": {"osc1": {"type": "va"}})");
        const Audio off =
            render(p, {note(0, 60, 1.0), cc(10, 64, 1.0), cc(4800, 120, 1.0)}, 24000, 256);
        CHECK(off.rms(12000, 24000) < 1e-6);
        const Audio held =
            render(p, {note(0, 60, 1.0), cc(10, 64, 1.0), cc(4800, 123, 1.0)}, 24000, 256);
        CHECK(held.rms(12000, 24000) > 0.01);
    }
    SUBCASE("a quality change keeps held notes sounding") {
        Engine engine;
        engine.prepare(48000.0, 256);
        engine.setPatch(parse(R"("oscillators": {"osc1": {"type": "va"}})"));
        engine.reset();
        std::vector<float> l(256), r(256);
        Event on;
        on.type = Event::Type::NoteOn;
        on.key = 60;
        on.value = 1.0;
        engine.process(std::span<const Event>(&on, 1), Transport{}, l.data(), r.data(), 256);
        engine.setPatch(
            parse(R"("voice": {"quality": "high"}, "oscillators": {"osc1": {"type": "va"}})"));
        double e = 0.0;
        for (int i = 0; i < 40; ++i) {
            engine.process({}, Transport{}, l.data(), r.data(), 256);
            for (float x : l) e += static_cast<double>(x) * static_cast<double>(x);
        }
        CHECK(engine.activeVoices() == 1);
        CHECK(e > 1.0);
    }
    SUBCASE("AM follows its via source") {
        const Patch p = parse(R"("oscillators": {"osc1": {"type": "va", "wave": "sine"},
                                                 "osc2": {"type": "va", "wave": "sine", "coarse": 7, "to": "none"}},
                                 "routes": {"osc2>osc1.am": {"amount": 100, "via": "modwheel"}})");
        const Audio dry = render(p, {note(0, 57, 1.0)}, 9600, 256);
        const Audio wet = render(p, {cc(0, 1, 1.0), note(0, 57, 1.0)}, 9600, 256);
        double diff = 0.0;
        for (std::size_t i = 2000; i < 9600; ++i)
            diff += std::fabs(static_cast<double>(dry.l[i] - wet.l[i]));
        CHECK(diff > 50.0);
    }
    SUBCASE("poly pressure reaches only the voice on its channel") {
        const Patch p = parse(R"("oscillators": {"osc1": {"type": "va"}},
                                 "filters": {"filter1": {"type": "svf", "cutoff": 200}},
                                 "routes": {"pressure>filter1.cutoff": {"amount": "+5 oct"}})");
        auto on = [](int ch) {
            Timed t = note(0, 60, 1.0);
            t.event.channel = static_cast<std::int16_t>(ch);
            return t;
        };
        auto pp = [](int ch) {
            Timed t{100, Event{}};
            t.event.type = Event::Type::PolyPressure;
            t.event.key = 60;
            t.event.channel = static_cast<std::int16_t>(ch);
            t.event.value = 1.0;
            return t;
        };
        const Audio one = render(p, {on(0), on(1), pp(0)}, 9600, 256);
        const Audio both = render(p, {on(0), on(1), pp(0), pp(1)}, 9600, 256);
        CHECK(one.rms(4800, 9600) < 0.9 * both.rms(4800, 9600));
    }
    SUBCASE("legato tracks 18 held keys; releasing the newest returns to the one before") {
        const Patch p = parse(
            R"("voice": {"mode": "legato"}, "oscillators": {"osc1": {"type": "va", "wave": "sine"}})");
        std::vector<Timed> ev;
        for (int k = 0; k < 18; ++k) ev.push_back(note(k * 10, 40 + k, 1.0));
        ev.push_back(noteOff(400, 57));
        // Key 56 (the 17th) sounds after releasing 57: its period is ~3.3 ms at 48 kHz.
        const Audio a = render(p, ev, 24000, 256);
        std::size_t crossings = 0;
        for (std::size_t i = 12001; i < 24000; ++i)
            crossings += (a.l[i - 1] < 0.0f) != (a.l[i] < 0.0f);
        const double hz = static_cast<double>(crossings) / 2.0 / 0.25;
        CHECK(hz == doctest::Approx(440.0 * std::pow(2.0, (56 - 69) / 12.0)).epsilon(0.03));
    }
}
