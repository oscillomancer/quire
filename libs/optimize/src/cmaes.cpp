#include "supple/opt/cmaes.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>

namespace supple::opt {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Cyclic Jacobi eigen-decomposition of a symmetric matrix: a = V·diag(w)·Vᵀ.
void jacobi(std::vector<Vec> a, Vec& w, std::vector<Vec>& v) {
    const std::size_t n = a.size();
    v.assign(n, Vec(n, 0.0));
    for (std::size_t i = 0; i < n; ++i) v[i][i] = 1.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (std::size_t p = 0; p < n; ++p)
            for (std::size_t q = p + 1; q < n; ++q) off += a[p][q] * a[p][q];
        if (off < 1e-30) break;
        for (std::size_t p = 0; p < n; ++p)
            for (std::size_t q = p + 1; q < n; ++q) {
                if (std::fabs(a[p][q]) < 1e-300) continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t =
                    (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (std::size_t k = 0; k < n; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    w.resize(n);
    for (std::size_t i = 0; i < n; ++i) w[i] = a[i][i];
}

double normal(Rng& rng) {
    // Box–Muller; one value per call keeps the stream order simple.
    double u = rng.uniform();
    while (u <= 1e-300) u = rng.uniform();
    const double v = rng.uniform();
    return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * kPi * v);
}

}  // namespace

std::string_view stopName(Stop s) {
    switch (s) {
        case Stop::None:
            return "none";
        case Stop::Evaluations:
            return "evaluations";
        case Stop::Time:
            return "time";
        case Stop::Target:
            return "target";
        case Stop::Stagnation:
            return "stagnation";
        case Stop::SigmaCollapse:
            return "sigma_collapse";
        case Stop::Cancelled:
            return "cancelled";
    }
    return "none";
}

double mirror01(double x) {
    double t = std::fmod(x, 2.0);
    if (t < 0.0) t += 2.0;
    return t > 1.0 ? 2.0 - t : t;
}

Cmaes::Cmaes(Vec mean, Options options)
    : n_(static_cast<int>(mean.size())), opt_(options), rng_(options.seed), mean_(std::move(mean)) {
    assert(n_ >= 1);
    const double n = n_;
    lambda_ = opt_.lambda > 0 ? opt_.lambda : 4 + static_cast<int>(std::floor(3.0 * std::log(n)));
    mu_ = lambda_ / 2;
    weights_.resize(static_cast<std::size_t>(mu_));
    for (int i = 0; i < mu_; ++i)
        weights_[static_cast<std::size_t>(i)] = std::log((lambda_ + 1.0) / 2.0) - std::log(i + 1.0);
    const double sum = std::accumulate(weights_.begin(), weights_.end(), 0.0);
    for (double& w : weights_) w /= sum;
    double sq = 0.0;
    for (double w : weights_) sq += w * w;
    mueff_ = 1.0 / sq;
    cs_ = (mueff_ + 2.0) / (n + mueff_ + 5.0);
    ds_ = 1.0 + 2.0 * std::max(0.0, std::sqrt((mueff_ - 1.0) / (n + 1.0)) - 1.0) + cs_;
    cc_ = (4.0 + mueff_ / n) / (n + 4.0 + 2.0 * mueff_ / n);
    c1_ = 2.0 / ((n + 1.3) * (n + 1.3) + mueff_);
    cmu_ =
        std::min(1.0 - c1_, 2.0 * (mueff_ - 2.0 + 1.0 / mueff_) / ((n + 2.0) * (n + 2.0) + mueff_));
    chiN_ = std::sqrt(n) * (1.0 - 1.0 / (4.0 * n) + 1.0 / (21.0 * n * n));
    sigma_ = opt_.sigma;
    pc_.assign(static_cast<std::size_t>(n_), 0.0);
    ps_.assign(static_cast<std::size_t>(n_), 0.0);
    d_.assign(static_cast<std::size_t>(n_), 1.0);
    c_.assign(static_cast<std::size_t>(n_), Vec(static_cast<std::size_t>(n_), 0.0));
    b_ = c_;
    for (std::size_t i = 0; i < static_cast<std::size_t>(n_); ++i) c_[i][i] = b_[i][i] = 1.0;
    eigenEvery_ =
        std::max(1, static_cast<int>(std::ceil(lambda_ / (c1_ + cmu_) / n / 10.0 / lambda_)));
}

void Cmaes::updateEigen() {
    Vec w;
    jacobi(c_, w, b_);
    for (std::size_t i = 0; i < w.size(); ++i) d_[i] = std::sqrt(std::max(w[i], 1e-20));
}

const std::vector<Vec>& Cmaes::ask() {
    const auto n = static_cast<std::size_t>(n_);
    y_.assign(static_cast<std::size_t>(lambda_), Vec(n));
    x_.assign(static_cast<std::size_t>(lambda_), Vec(n));
    Vec z(n);
    for (std::size_t k = 0; k < static_cast<std::size_t>(lambda_); ++k) {
        for (std::size_t i = 0; i < n; ++i) z[i] = normal(rng_) * d_[i];
        for (std::size_t i = 0; i < n; ++i) {
            double s = 0.0;
            for (std::size_t j = 0; j < n; ++j) s += b_[i][j] * z[j];
            y_[k][i] = s;
            const double g = mean_[i] + sigma_ * s;
            x_[k][i] = opt_.bounded ? mirror01(g) : g;
        }
    }
    return x_;
}

void Cmaes::tell(std::span<const double> losses) {
    const auto n = static_cast<std::size_t>(n_);
    assert(losses.size() == static_cast<std::size_t>(lambda_));
    std::vector<std::size_t> order(static_cast<std::size_t>(lambda_));
    std::iota(order.begin(), order.end(), 0);
    auto key = [&](std::size_t i) {
        const double l = losses[i];
        return std::isfinite(l) ? l : std::numeric_limits<double>::infinity();
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return key(a) < key(b); });
    // Weighted recombination of the best μ steps.
    Vec yw(n, 0.0);
    for (int i = 0; i < mu_; ++i)
        for (std::size_t j = 0; j < n; ++j)
            yw[j] +=
                weights_[static_cast<std::size_t>(i)] * y_[order[static_cast<std::size_t>(i)]][j];
    for (std::size_t j = 0; j < n; ++j) mean_[j] += sigma_ * yw[j];
    // C^{-1/2}·yw = B·D⁻¹·Bᵀ·yw.
    Vec t(n, 0.0), cinv(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double s = 0.0;
        for (std::size_t j = 0; j < n; ++j) s += b_[j][i] * yw[j];
        t[i] = s / d_[i];
    }
    for (std::size_t i = 0; i < n; ++i) {
        double s = 0.0;
        for (std::size_t j = 0; j < n; ++j) s += b_[i][j] * t[j];
        cinv[i] = s;
    }
    const double csn = std::sqrt(cs_ * (2.0 - cs_) * mueff_);
    for (std::size_t i = 0; i < n; ++i) ps_[i] = (1.0 - cs_) * ps_[i] + csn * cinv[i];
    double psNorm = 0.0;
    for (double v : ps_) psNorm += v * v;
    psNorm = std::sqrt(psNorm);
    ++generation_;
    const double hsig = psNorm / std::sqrt(1.0 - std::pow(1.0 - cs_, 2.0 * generation_)) <
                                (1.4 + 2.0 / (n_ + 1.0)) * chiN_
                            ? 1.0
                            : 0.0;
    const double ccn = std::sqrt(cc_ * (2.0 - cc_) * mueff_);
    for (std::size_t i = 0; i < n; ++i) pc_[i] = (1.0 - cc_) * pc_[i] + hsig * ccn * yw[i];
    const double deltaH = (1.0 - hsig) * cc_ * (2.0 - cc_);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j <= i; ++j) {
            double rankMu = 0.0;
            for (int k = 0; k < mu_; ++k) {
                const Vec& y = y_[order[static_cast<std::size_t>(k)]];
                rankMu += weights_[static_cast<std::size_t>(k)] * y[i] * y[j];
            }
            const double v = (1.0 - c1_ - cmu_) * c_[i][j] +
                             c1_ * (pc_[i] * pc_[j] + deltaH * c_[i][j]) + cmu_ * rankMu;
            c_[i][j] = c_[j][i] = v;
        }
    sigma_ *= std::exp((cs_ / ds_) * (psNorm / chiN_ - 1.0));
    if (++eigenEval_ >= eigenEvery_) {
        eigenEval_ = 0;
        updateEigen();
    }
    // Convergence bookkeeping.
    const double best = key(order[0]);
    // (The first finite loss always counts: tolerance × ∞ would be NaN.)
    if (!std::isfinite(bestSeen_) ||
        best < bestSeen_ - opt_.stagnationTolerance * std::max(1e-12, std::fabs(bestSeen_))) {
        bestSeen_ = best;
        sinceImprovement_ = 0;
    } else {
        ++sinceImprovement_;
    }
    const int patience = 10 + static_cast<int>(std::ceil(30.0 * n_ / lambda_));
    double maxD = 0.0;
    for (std::size_t i = 0; i < n; ++i) maxD = std::max(maxD, std::sqrt(c_[i][i]));
    if (sigma_ * maxD < opt_.sigmaTolerance)
        converged_ = Stop::SigmaCollapse;
    else if (sinceImprovement_ >= patience)
        converged_ = Stop::Stagnation;
}

Outcome minimize(const BatchLoss& loss, Vec x0, const Options& options,
                 const std::function<bool(const Progress&)>& onProgress) {
    const auto started = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    Outcome out;
    if (options.bounded)
        for (double& v : x0) v = mirror01(v);
    const std::vector<double> first = loss({x0});
    out.initialLoss = first.empty() ? std::numeric_limits<double>::infinity() : first[0];
    out.best = x0;
    out.bestLoss =
        std::isfinite(out.initialLoss) ? out.initialLoss : std::numeric_limits<double>::infinity();
    out.evaluations = 1;
    Cmaes es(x0, options);
    while (true) {
        if (out.bestLoss <= options.targetLoss) {
            out.stopped = Stop::Target;
            break;
        }
        if (out.evaluations + es.lambda() > options.maxEvaluations) {
            out.stopped = Stop::Evaluations;
            break;
        }
        if (elapsed() >= options.maxSeconds) {
            out.stopped = Stop::Time;
            break;
        }
        const std::vector<Vec>& pop = es.ask();
        const std::vector<double> losses = loss(pop);
        out.evaluations += static_cast<int>(pop.size());
        for (std::size_t k = 0; k < pop.size() && k < losses.size(); ++k)
            if (std::isfinite(losses[k]) && losses[k] < out.bestLoss) {
                out.bestLoss = losses[k];
                out.best = pop[k];
            }
        es.tell(losses);
        out.generations = es.generation();
        if (onProgress &&
            !onProgress({out.evaluations, es.generation(), out.bestLoss, es.sigma()})) {
            out.stopped = Stop::Cancelled;
            break;
        }
        if (es.converged() != Stop::None) {
            out.stopped = es.converged();
            break;
        }
    }
    out.seconds = elapsed();
    return out;
}

}  // namespace supple::opt
