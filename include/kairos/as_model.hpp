#pragma once

// Avellaneda-Stoikov (2008) market-making model: optimal quotes.
//
// A market maker with inventory q and risk aversion gamma sets:
//   reservation price:  r = s - q * gamma * sigma^2 * (T - t)
//   bid distance:       d_bid = (1/gamma)*ln(1 + gamma/kappa)
//                       + gamma*sigma^2*(T-t) * (q + 1/2)
//   ask distance:       d_ask = (1/gamma)*ln(1 + gamma/kappa)
//                       + gamma*sigma^2*(T-t) * (-q + 1/2)
//   bid = r - d_bid,  ask = r + d_ask
//
// Term by term:
//   s:            mid price (ticks). The frictionless reference.
//   q:            inventory in lots, signed (+ = long). The model skews
//                 quotes to mean-revert inventory toward zero.
//   gamma:        risk-aversion parameter (> 0). Higher gamma = wider
//                 spreads, stronger inventory skew. Units absorb the
//                 lot/tick scaling; calibrate, don't interpret absolutely.
//   sigma:        volatility per sqrt(second) of the mid price (relative,
//                 i.e. std of log returns). Estimated from data.
//   kappa:        order-book liquidity: the arrival rate of market orders
//                 at distance d from the touch decays as A*exp(-kappa*d).
//                 Higher kappa = thinner book = wider optimal spread.
//   (T - t):      remaining time in the trading horizon (seconds). As the
//                 horizon shrinks, spreads tighten toward (1/gamma)*ln(...)
//                 and the inventory skew vanishes (no time left to care).
//
// All model math is double (strategy research, not the matching core).
// Quotes are rounded to integer ticks. Price/qty in the engine stay
// integer; this header is the boundary where quant math meets ticks.

#include <cmath>
#include <cstdint>
#include <vector>

namespace kairos {

struct ASParams {
    double gamma = 0.05;     // risk aversion
    double sigma = 0.0002;   // vol per sqrt(sec), relative
    double kappa = 1.5;      // book liquidity (per tick)
    double horizon_s = 3600.0;  // trading horizon T, seconds
};

struct ASQuotes {
    std::int64_t bid_ticks = 0;
    std::int64_t ask_ticks = 0;
    double reservation_ticks = 0.0;  // for diagnostics
};

// Compute optimal quotes. elapsed_s: seconds since horizon start.
// inventory_lots: signed. Returns bid < ask (clamped to >= 1 tick spread).
inline ASQuotes avellaneda_stoikov_quotes(const ASParams& p,
                                          std::int64_t mid_ticks,
                                          std::int64_t inventory_lots,
                                          double elapsed_s) {
    ASQuotes out;
    const double t_rem = p.horizon_s - elapsed_s > 0.0
                             ? p.horizon_s - elapsed_s
                             : 0.0;
    const double q = static_cast<double>(inventory_lots);
    const double s = static_cast<double>(mid_ticks);

    // Reservation price: where I'd trade to flatten inventory now.
    const double r = s - q * p.gamma * p.sigma * p.sigma * t_rem;

    // Optimal distances from the reservation price.
    const double base = std::log(1.0 + p.gamma / p.kappa) / p.gamma;
    const double tadJ = p.gamma * p.sigma * p.sigma * t_rem;
    double d_bid = base + tadJ * (q + 0.5);
    double d_ask = base + tadJ * (-q + 0.5);
    // Distances must stay positive; extreme inventory can push them
    // negative (model says "quote through the touch"), which we clamp.
    if (d_bid < 1.0) {
        d_bid = 1.0;
    }
    if (d_ask < 1.0) {
        d_ask = 1.0;
    }

    out.reservation_ticks = r;
    out.bid_ticks = static_cast<std::int64_t>(std::llround(r - d_bid));
    out.ask_ticks = static_cast<std::int64_t>(std::llround(r + d_ask));
    if (out.bid_ticks >= out.ask_ticks) {
        out.ask_ticks = out.bid_ticks + 1;
    }
    return out;
}

// Estimate sigma (per sqrt-second, relative) from mid-price ticks sampled
// at irregular times. Uses mean(r_i^2 / dt_i) where r_i are log returns.
inline double estimate_sigma(const std::int64_t* mid_ticks,
                             const std::uint64_t* ts_ns, std::size_t n) {
    if (n < 3) {
        return 0.0;
    }
    double sum = 0.0;
    std::size_t cnt = 0;
    for (std::size_t i = 1; i < n; ++i) {
        if (mid_ticks[i - 1] <= 0 || mid_ticks[i] <= 0) {
            continue;
        }
        const double dt =
            static_cast<double>(ts_ns[i] - ts_ns[i - 1]) / 1e9;
        if (dt <= 0.0) {
            continue;
        }
        const double r = std::log(static_cast<double>(mid_ticks[i]) /
                                  static_cast<double>(mid_ticks[i - 1]));
        sum += (r * r) / dt;
        ++cnt;
    }
    if (cnt == 0) {
        return 0.0;
    }
    return std::sqrt(sum / static_cast<double>(cnt));
}

// Estimate kappa from trades. Each trade contributes its distance from the
// mid (in ticks, >= 0) at trade time. The A-S assumption is
// rate(d) = A * exp(-kappa * d); we bin distances, compute per-bin rates,
// and fit ln(rate) = ln(A) - kappa * d by least squares. Returns -1 if
// the fit is degenerate (too few bins or no variation).
inline double estimate_kappa(const double* dist_ticks, std::size_t n,
                             double total_time_s, std::size_t n_bins = 20) {
    if (n < 10 || total_time_s <= 0.0 || n_bins < 2) {
        return -1.0;
    }
    double dmax = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (dist_ticks[i] > dmax) {
            dmax = dist_ticks[i];
        }
    }
    if (dmax <= 0.0) {
        return -1.0;
    }
    std::vector<std::size_t> counts(n_bins, 0);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t b =
            static_cast<std::size_t>(dist_ticks[i] / dmax * n_bins);
        if (b >= n_bins) {
            b = n_bins - 1;
        }
        ++counts[b];
    }
    // Least squares on (bin_center, ln(rate)) for non-empty bins.
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    std::size_t m = 0;
    for (std::size_t b = 0; b < n_bins; ++b) {
        if (counts[b] == 0) {
            continue;
        }
        const double x = (static_cast<double>(b) + 0.5) / n_bins * dmax;
        const double y =
            std::log(static_cast<double>(counts[b]) / total_time_s);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++m;
    }
    if (m < 3) {
        return -1.0;
    }
    const double denom = static_cast<double>(m) * sxx - sx * sx;
    if (denom <= 0.0) {
        return -1.0;
    }
    const double slope = (static_cast<double>(m) * sxy - sx * sy) / denom;
    // slope should be negative; kappa = -slope.
    return slope < 0.0 ? -slope : -1.0;
}

}  // namespace kairos
