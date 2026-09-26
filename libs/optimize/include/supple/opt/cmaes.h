// Derivative-free minimization: (μ/μ_w, λ)-CMA-ES (spec: docs/specs/optimizer.md
// §1), implemented from the equations of N. Hansen, "The CMA Evolution
// Strategy: A Tutorial" (arXiv:1604.00772), default strategy parameters.
// Optional box bounds [0, 1] by mirroring (the unmirrored genotype stays in
// the distribution). Deterministic for a given seed. No audio knowledge.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "supple/base/rng.h"

namespace supple::opt {

using Vec = std::vector<double>;

enum class Stop { None, Evaluations, Time, Target, Stagnation, SigmaCollapse, Cancelled };
std::string_view stopName(Stop s);

struct Options {
    double sigma = 0.2;  // initial step size (normalized domain)
    int lambda = 0;      // population; 0 = 4 + ⌊3 ln n⌋
    std::uint64_t seed = 0;
    bool bounded = true;  // mirror candidates into [0, 1]
    int maxEvaluations = 300;
    double maxSeconds = 60.0;
    double targetLoss = -std::numeric_limits<double>::infinity();
    double stagnationTolerance = 1e-4;  // relative best-loss improvement
    double sigmaTolerance = 1e-4;       // stop when σ·max(√diag C) falls below
};

class Cmaes {
  public:
    Cmaes(Vec mean, Options options);

    // The next population (phenotypes; mirrored into [0, 1] when bounded).
    const std::vector<Vec>& ask();
    // Losses for the population returned by ask(), in order. Non-finite
    // losses rank last.
    void tell(std::span<const double> losses);

    int dimension() const { return n_; }
    int lambda() const { return lambda_; }
    int generation() const { return generation_; }
    double sigma() const { return sigma_; }
    const Vec& mean() const { return mean_; }
    // Stagnation or σ collapse detected by tell().
    Stop converged() const { return converged_; }

  private:
    void updateEigen();

    int n_, lambda_, mu_;
    Options opt_;
    Rng rng_;
    Vec mean_, weights_, pc_, ps_, d_;
    std::vector<Vec> c_, b_;  // covariance, eigenvectors (columns)
    std::vector<Vec> y_, x_;  // current population: steps and phenotypes
    double sigma_, mueff_, cs_, ds_, cc_, c1_, cmu_, chiN_;
    int generation_ = 0, eigenEval_ = 0, eigenEvery_ = 1;
    double bestSeen_ = std::numeric_limits<double>::infinity();
    int sinceImprovement_ = 0;
    Stop converged_ = Stop::None;
};

struct Progress {
    int evaluations = 0, generation = 0;
    double bestLoss = 0.0, sigma = 0.0;
};

struct Outcome {
    Vec best;
    double bestLoss = std::numeric_limits<double>::infinity();
    double initialLoss = std::numeric_limits<double>::infinity();
    int evaluations = 0, generations = 0;
    Stop stopped = Stop::None;
    double seconds = 0.0;
};

// Evaluates a batch of points (may run them in parallel; results by index).
using BatchLoss = std::function<std::vector<double>(const std::vector<Vec>&)>;
// Minimizes from x0 (evaluated first). onProgress (after every generation)
// returns false to cancel.
Outcome minimize(const BatchLoss& loss, Vec x0, const Options& options,
                 const std::function<bool(const Progress&)>& onProgress = {});

// Mirrors x into [0, 1] (periodic reflection).
double mirror01(double x);

}  // namespace supple::opt
