#include <doctest/doctest.h>
#include <supple/base/rng.h>
#include <supple/time/timeline.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace supple;
using namespace supple::time;

namespace {

// Seconds from beat 0 to `beat` by Simpson integration of 60 / bpm(b), piece
// by piece between tempo points (the integrand jumps at step changes).
double simpson(const TimeMap& m, double a, double b) {
    const int n = 2000;
    const double h = (b - a) / n;
    // Evaluate just inside the piece so a step change at `b` is not sampled.
    auto f = [&](double x) { return 60.0 / m.bpmAt(std::min(x, std::nextafter(b, a))); };
    double sum = f(a) + f(b);
    for (int i = 1; i < n; ++i) sum += (i % 2 ? 4.0 : 2.0) * f(a + i * h);
    return sum * h / 3.0;
}
double integrate(const TimeMap& m, double beat) {
    double total = 0.0, from = 0.0;
    for (const auto& p : m.tempo()) {
        if (p.beat <= from) continue;
        if (p.beat >= beat) break;
        total += simpson(m, from, p.beat);
        from = p.beat;
    }
    return total + simpson(m, from, beat);
}

TimeMap randomMap(Rng& rng) {
    std::vector<TempoPoint> t;
    double beat = 0.0;
    const int n = 1 + static_cast<int>(rng.below(8));
    for (int i = 0; i < n; ++i) {
        t.push_back(
            {beat, rng.uniform(30.0, 300.0), rng.below(2) ? TempoShape::Ramp : TempoShape::Step});
        beat += rng.uniform(0.25, 32.0);
    }
    std::vector<MeterChange> m{{1, 4, 4}};
    int bar = 1;
    for (int i = 0; i < static_cast<int>(rng.below(4)); ++i) {
        bar += 1 + static_cast<int>(rng.below(8));
        const int dens[] = {2, 4, 8, 16};
        m.push_back({bar, 1 + static_cast<int>(rng.below(12)), dens[rng.below(4)]});
    }
    return TimeMap::create(t, m).value();
}

}  // namespace

TEST_CASE("constant and stepped tempo") {
    const TimeMap c = TimeMap::constant(120.0);
    CHECK(c.secondsAt(4.0) == 2.0);
    CHECK(c.beatAt(2.0) == 4.0);
    CHECK(c.secondsAt(-2.0) == -1.0);  // before the start the first tempo holds
    auto m = TimeMap::create({{0, 120}, {4, 60}}, {{1, 4, 4}});
    REQUIRE(m);
    CHECK(m->secondsAt(4.0) == 2.0);
    CHECK(m->secondsAt(6.0) == 4.0);
    CHECK(m->beatAt(4.0) == 6.0);
    CHECK(m->bpmAt(3.9) == 120.0);
    CHECK(m->bpmAt(4.0) == 60.0);
}

TEST_CASE("ramps are linear in beats and match numeric integration") {
    auto m = TimeMap::create({{0, 60, TempoShape::Ramp}, {4, 120}}, {{1, 4, 4}});
    REQUIRE(m);
    CHECK(m->bpmAt(2.0) == doctest::Approx(90.0));
    CHECK(m->secondsAt(4.0) == doctest::Approx(4.0 * std::log(2.0)).epsilon(1e-14));
    CHECK(m->secondsAt(4.0) == doctest::Approx(integrate(*m, 4.0)).epsilon(1e-12));
    CHECK(m->bpmAt(10.0) == 120.0);  // the last point holds
    // A ramp between nearly equal tempos keeps full precision.
    auto flat = TimeMap::create({{0, 120, TempoShape::Ramp}, {8, 120.000000001}}, {{1, 4, 4}});
    REQUIRE(flat);
    CHECK(flat->secondsAt(8.0) == doctest::Approx(integrate(*flat, 8.0)).epsilon(1e-14));
    CHECK(flat->secondsAt(8.0) < 4.0);  // slightly faster than 120 throughout
    CHECK(flat->beatAt(flat->secondsAt(5.0)) == doctest::Approx(5.0).epsilon(1e-14));
}

TEST_CASE("random maps: exact inverses, monotonic, integration agrees") {
    Rng rng(1234);
    for (int trial = 0; trial < 300; ++trial) {
        const TimeMap m = randomMap(rng);
        double prev = -1e300;
        for (int i = 0; i < 200; ++i) {
            const double b = rng.uniform(-10.0, 300.0);
            const double t = m.secondsAt(b);
            CHECK(std::fabs(m.beatAt(t) - b) < 1e-9);
            (void)prev;
        }
        for (double b = -4.0; b < 250.0; b += 0.37) {
            const double t = m.secondsAt(b);
            CHECK(t > prev);
            prev = t;
        }
        const double end = m.tempo().back().beat + 3.0;
        CHECK(m.secondsAt(end) == doctest::Approx(integrate(m, end)).epsilon(1e-9));
    }
}

TEST_CASE("meters: bar starts, bar/beat positions in denominator units, pickups") {
    auto m = TimeMap::create({{0, 120}}, {{1, 4, 4}, {3, 3, 4}, {5, 6, 8}});
    REQUIRE(m);
    CHECK(m->beatOfBar(1) == 0.0);
    CHECK(m->beatOfBar(2) == 4.0);
    CHECK(m->beatOfBar(3) == 8.0);
    CHECK(m->beatOfBar(4) == 11.0);
    CHECK(m->beatOfBar(5) == 14.0);
    CHECK(m->beatOfBar(6) == 17.0);
    CHECK(m->beatOfBar(0) == -4.0);  // a pickup bar extends the first meter
    CHECK(m->barLength(5) == 3.0);
    CHECK(m->meterAt(6) == MeterChange{5, 6, 8});

    BarBeat p = m->barBeat(11.5);
    CHECK(p.bar == 4);
    CHECK(p.beat == 1);
    CHECK(p.fraction == 0.5);
    p = m->barBeat(14.75);  // 6/8: beats are eighths
    CHECK(p.bar == 5);
    CHECK(p.beat == 2);
    CHECK(p.fraction == doctest::Approx(0.5));
    CHECK(p.quarters == 0.75);
    p = m->barBeat(-1.0);
    CHECK(p.bar == 0);
    CHECK(p.beat == 4);
    // Just before a bar line is on it — including where the meter changes.
    p = m->barBeat(4.0 - 1e-12);
    CHECK(p.bar == 2);
    CHECK(p.beat == 1);
    CHECK(p.fraction == 0.0);
    p = m->barBeat(14.0 - 1e-12);
    CHECK(p.bar == 5);
    CHECK(p.beat == 1);

    Rng rng(7);
    for (int i = 0; i < 2000; ++i) {
        const double b = rng.uniform(-8.0, 100.0);
        CHECK(m->beatOf(m->barBeat(b)) == doctest::Approx(b).epsilon(1e-12));
    }
}

TEST_CASE("validation errors carry paths") {
    auto e = TimeMap::create({{1, 120}}, {{1, 4, 4}});
    REQUIRE(!e);
    CHECK(e.error().path == "/tempo/0/beat");
    e = TimeMap::create({{0, 120}, {0, 90}}, {{1, 4, 4}});
    REQUIRE(!e);
    CHECK(e.error().path == "/tempo/1/beat");
    e = TimeMap::create({{0, 0.5}}, {{2, 4, 3}});
    REQUIRE(!e);
    REQUIRE(e.error().details.size() == 3);
    CHECK(e.error().details[0].path == "/tempo/0/bpm");
    CHECK(e.error().details[1].path == "/meter/0/bar");
    CHECK(e.error().details[2].path == "/meter/0/denominator");
    CHECK(TimeMap::constant(0.0) == TimeMap::constant(120.0));  // invalid -> 120 BPM 4/4
}

TEST_CASE("samples round half to even") {
    CHECK(samplesAt(0.25, 2.0) == 0);  // 0.5
    CHECK(samplesAt(0.75, 2.0) == 2);  // 1.5
    CHECK(samplesAt(1.25, 2.0) == 2);  // 2.5
    CHECK(samplesAt(-0.75, 2.0) == -2);
    CHECK(samplesAt(1.0, 48000.0) == 48000);
}

TEST_CASE("pickup positions do not overflow integer subtraction") {
    const auto map = TimeMap::constant(120);
    constexpr int first = std::numeric_limits<int>::min();
    CHECK(map.beatOfBar(first) == (static_cast<double>(first) - 1) * 4);
    CHECK(map.beatOf({1, first}) == static_cast<double>(first) - 1);
}
