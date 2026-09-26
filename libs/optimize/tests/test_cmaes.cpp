#include <doctest/doctest.h>

#include <cmath>

#include "supple/opt/cmaes.h"

using namespace supple;
using namespace supple::opt;

namespace {

BatchLoss batch(double (*f)(const Vec&)) {
    return [f](const std::vector<Vec>& xs) {
        std::vector<double> out;
        for (const Vec& x : xs) out.push_back(f(x));
        return out;
    };
}

double sphere(const Vec& x) {
    double s = 0.0;
    for (double v : x) s += v * v;
    return s;
}

double ellipsoid(const Vec& x) {  // condition number 1e6
    double s = 0.0;
    const double n = static_cast<double>(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        s += std::pow(1e6, static_cast<double>(i) / (n - 1)) * x[i] * x[i];
    return s;
}

double rosenbrock(const Vec& x) {
    double s = 0.0;
    for (std::size_t i = 0; i + 1 < x.size(); ++i)
        s += 100.0 * std::pow(x[i + 1] - x[i] * x[i], 2) + std::pow(1.0 - x[i], 2);
    return s;
}

Options unbounded(int evaluations, double target) {
    Options o;
    o.bounded = false;
    o.sigma = 0.5;
    o.maxEvaluations = evaluations;
    o.maxSeconds = 120.0;
    o.targetLoss = target;
    o.stagnationTolerance = 0.0;
    o.sigmaTolerance = 1e-14;
    return o;
}

}  // namespace

TEST_CASE("standard functions reach their targets within published budgets (+50 %)") {
    SUBCASE("sphere n=10") {
        const Outcome r = minimize(batch(sphere), Vec(10, 1.0), unbounded(3000, 1e-10));
        CHECK(r.stopped == Stop::Target);
        CHECK(r.bestLoss <= 1e-10);
    }
    SUBCASE("ellipsoid n=10") {
        const Outcome r = minimize(batch(ellipsoid), Vec(10, 1.0), unbounded(12000, 1e-10));
        CHECK(r.stopped == Stop::Target);
    }
    SUBCASE("rosenbrock n=10") {
        const Outcome r = minimize(batch(rosenbrock), Vec(10, 0.0), unbounded(15000, 1e-10));
        CHECK(r.stopped == Stop::Target);
        for (double v : r.best) CHECK(v == doctest::Approx(1.0).epsilon(1e-3));
    }
}

TEST_CASE("bounds: optima inside and outside the unit box") {
    Options o;
    o.maxEvaluations = 2000;
    o.targetLoss = 1e-12;
    auto inside = [](const std::vector<Vec>& xs) {
        std::vector<double> out;
        for (const Vec& x : xs) {
            double s = 0.0;
            for (double v : x) s += (v - 0.9) * (v - 0.9);
            out.push_back(s);
        }
        return out;
    };
    const Outcome a = minimize(inside, Vec(4, 0.1), o);
    for (double v : a.best) CHECK(v == doctest::Approx(0.9).epsilon(1e-4));
    auto outside = [](const std::vector<Vec>& xs) {
        std::vector<double> out;
        for (const Vec& x : xs) {
            double s = 0.0;
            for (double v : x) {
                CHECK(v >= 0.0);
                CHECK(v <= 1.0);
                s += (v - 1.5) * (v - 1.5);
            }
            out.push_back(s);
        }
        return out;
    };
    o.targetLoss = -1.0;
    const Outcome b = minimize(outside, Vec(3, 0.5), o);
    for (double v : b.best) CHECK(v == doctest::Approx(1.0).epsilon(1e-3));
}

TEST_CASE("deterministic for a seed; different seeds explore differently") {
    Options o = unbounded(600, 0.0);
    o.seed = 42;
    const Outcome a = minimize(batch(rosenbrock), Vec(5, 0.0), o);
    const Outcome b = minimize(batch(rosenbrock), Vec(5, 0.0), o);
    CHECK(a.best == b.best);
    CHECK(a.bestLoss == b.bestLoss);
    o.seed = 43;
    const Outcome c = minimize(batch(rosenbrock), Vec(5, 0.0), o);
    CHECK(c.best != a.best);
}

TEST_CASE("stopping: evaluations, cancellation, stagnation") {
    Options o = unbounded(100, -1.0);
    const Outcome r = minimize(batch(sphere), Vec(6, 1.0), o);
    CHECK(r.stopped == Stop::Evaluations);
    CHECK(r.evaluations <= 100);
    int calls = 0;
    const Outcome c = minimize(batch(sphere), Vec(6, 1.0), unbounded(1000, -1.0),
                               [&](const Progress&) { return ++calls < 3; });
    CHECK(c.stopped == Stop::Cancelled);
    CHECK(calls == 3);
    // A flat function stagnates.
    Options f;
    f.maxEvaluations = 100000;
    const Outcome s =
        minimize([](const std::vector<Vec>& xs) { return std::vector<double>(xs.size(), 1.0); },
                 Vec(3, 0.5), f);
    CHECK(s.stopped == Stop::Stagnation);
    CHECK(mirror01(1.25) == doctest::Approx(0.75));
    CHECK(mirror01(-0.25) == doctest::Approx(0.25));
    CHECK(mirror01(2.5) == doctest::Approx(0.5));
}
