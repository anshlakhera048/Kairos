// Tests for the Avellaneda-Stoikov model: quote math and estimators.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "kairos/as_model.hpp"

namespace kairos {
namespace {

TEST(ASModel, SymmetricAtZeroInventory) {
    ASParams p;
    p.gamma = 1500.0;  // O(1000): sigma^2*T is O(1e-4), so gamma must be
    p.sigma = 0.0002;  // large for tick-level inventory skew. This is a
    p.kappa = 1.0;     // calibration reality, not a model bug.
    p.horizon_s = 3600.0;
    const auto q = avellaneda_stoikov_quotes(p, 100000, 0, 0.0);
    EXPECT_LT(q.bid_ticks, 100000);
    EXPECT_GT(q.ask_ticks, 100000);
    // Symmetric around mid at zero inventory.
    EXPECT_EQ(q.bid_ticks - 100000, 100000 - q.ask_ticks);
    EXPECT_NEAR(q.reservation_ticks, 100000.0, 1e-9);
}

TEST(ASModel, InventorySkewsQuotes) {
    ASParams p;
    p.gamma = 1500.0;
    p.sigma = 0.0002;
    p.kappa = 1.0;
    p.horizon_s = 3600.0;
    const auto flat = avellaneda_stoikov_quotes(p, 100000, 0, 0.0);
    // Long inventory: reservation below mid, bid pushed down, ask pulled in.
    const auto long_q = avellaneda_stoikov_quotes(p, 100000, 50, 0.0);
    EXPECT_LT(long_q.reservation_ticks, 100000.0);
    EXPECT_LT(long_q.bid_ticks, flat.bid_ticks);
    EXPECT_LT(long_q.ask_ticks, flat.ask_ticks);
    // Short inventory: mirror image.
    const auto short_q = avellaneda_stoikov_quotes(p, 100000, -50, 0.0);
    EXPECT_GT(short_q.reservation_ticks, 100000.0);
    EXPECT_GT(short_q.bid_ticks, flat.bid_ticks);
    EXPECT_GT(short_q.ask_ticks, flat.ask_ticks);
}

TEST(ASModel, SpreadTightensIntoHorizon) {
    ASParams p;
    p.gamma = 1500.0;
    p.sigma = 0.0002;
    p.kappa = 1.0;
    p.horizon_s = 3600.0;
    const auto early = avellaneda_stoikov_quotes(p, 100000, 10, 0.0);
    const auto late = avellaneda_stoikov_quotes(p, 100000, 10, 3599.0);
    EXPECT_GT(early.ask_ticks - early.bid_ticks,
              late.ask_ticks - late.bid_ticks);
}

TEST(ASModel, HigherGammaWidensSpread) {
    // Use high sigma so spreads exceed one tick and gamma's effect is
    // visible after rounding.
    ASParams low;
    low.gamma = 100.0;
    low.sigma = 0.01;
    low.kappa = 1.0;
    low.horizon_s = 3600.0;
    ASParams high;
    high.gamma = 5000.0;
    high.sigma = 0.01;
    high.kappa = 1.0;
    high.horizon_s = 3600.0;
    const auto qlow = avellaneda_stoikov_quotes(low, 100000, 0, 0.0);
    const auto qhigh = avellaneda_stoikov_quotes(high, 100000, 0, 0.0);
    EXPECT_GT(qhigh.ask_ticks - qhigh.bid_ticks,
              qlow.ask_ticks - qlow.bid_ticks);
}

TEST(ASModel, EstimateSigmaRecoversTruth) {
    // Geometric Brownian motion with known sigma.
    const double true_sigma = 0.0003;  // per sqrt(sec)
    const double dt = 1.0;             // 1-second steps
    std::mt19937_64 rng(42);
    std::normal_distribution<double> norm;
    const int n = 20000;
    std::vector<std::int64_t> mids;
    std::vector<std::uint64_t> ts;
    mids.reserve(n);
    ts.reserve(n);
    double logp = std::log(100000.0);
    for (int i = 0; i < n; ++i) {
        logp += true_sigma * std::sqrt(dt) * norm(rng);
        mids.push_back(static_cast<std::int64_t>(std::llround(std::exp(logp))));
        ts.push_back(static_cast<std::uint64_t>(i) * 1000000000ULL);
    }
    const double est = estimate_sigma(mids.data(), ts.data(), mids.size());
    // Within 10% of truth with 20k samples.
    EXPECT_NEAR(est, true_sigma, true_sigma * 0.10);
}

TEST(ASModel, EstimateKappaRecoversTruth) {
    // Trades with P(distance) proportional to exp(-kappa * d).
    const double true_kappa = 0.8;
    std::mt19937_64 rng(1234);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const int n = 20000;
    const double dmax = 20.0;
    std::vector<double> dists;
    dists.reserve(n);
    // Inverse-CDF sampling from the truncated exponential.
    const double cdf_max = 1.0 - std::exp(-true_kappa * dmax);
    for (int i = 0; i < n; ++i) {
        const double u = uni(rng) * cdf_max;
        dists.push_back(-std::log(1.0 - u) / true_kappa);
    }
    const double est = estimate_kappa(dists.data(), dists.size(), 3600.0, 20);
    EXPECT_GT(est, 0.0);
    // Binned regression is noisy; accept 30% tolerance.
    EXPECT_NEAR(est, true_kappa, true_kappa * 0.30);
}

TEST(ASModel, EstimateKappaDegenerate) {
    // All trades at the same distance: no variation to fit.
    std::vector<double> dists(100, 5.0);
    EXPECT_LT(estimate_kappa(dists.data(), dists.size(), 3600.0), 0.0);
    EXPECT_LT(estimate_kappa(nullptr, 0, 3600.0), 0.0);
}

}  // namespace
}  // namespace kairos
