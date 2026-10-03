#pragma once

// Pluggable latency models for the simulator.
//
// Two latencies are modelled SEPARATELY:
//   - feed latency: exchange -> me (market data delay)
//   - order latency: me -> exchange (order/cancel acknowledgement delay)
//
// Models are concrete types satisfying the LatencyModel concept; the
// simulator is templated on them, so there are no virtual calls in the
// hot loop. All sampling is deterministic given the RNG state.
//
// Variants:
//   - ConstantLatency: fixed delay.
//   - JitteredLatency: base + uniform jitter in [0, jitter].
//   - EmpiricalLatency: sampled from measured data (the recorder captures
//     local_mono_ns - exchange_ts_ns per event; see tools/recorder).
//
// Usage in the simulator:
//   - A market event with exchange_ts T becomes visible to the strategy
//     at T + feed.sample(rng).
//   - A strategy action at local time T' takes effect at the exchange at
//     T' + order.sample(rng). Cancels race with fills on exchange time.

#include <concepts>
#include <cstdint>
#include <vector>

namespace kairos {

// Deterministic RNG (splitmix64). State is passed by reference; the caller
// owns it, so streams are reproducible and independent.
inline std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

template <typename L>
concept LatencyModel = requires(const L& m, std::uint64_t& rng) {
    { m.sample(rng) } -> std::same_as<std::uint64_t>;
};

// Fixed delay, every sample identical.
struct ConstantLatency {
    std::uint64_t ns = 0;
    constexpr std::uint64_t sample(std::uint64_t&) const noexcept { return ns; }
};

// Base delay plus uniform jitter in [0, jitter_ns].
struct JitteredLatency {
    std::uint64_t base_ns = 0;
    std::uint64_t jitter_ns = 0;
    std::uint64_t sample(std::uint64_t& rng) const noexcept {
        if (jitter_ns == 0) {
            return base_ns;
        }
        return base_ns + splitmix64(rng) % (jitter_ns + 1);
    }
};

// Sampled from measured latencies. Construction buckets the samples;
// sampling picks a bucket weighted by count, then a uniform offset
// within the bucket.
class EmpiricalLatency {
public:
    EmpiricalLatency() = default;

    // samples: measured latencies in ns (any order); n_buckets: histogram
    // resolution. Empty input -> always returns 0.
    EmpiricalLatency(const std::uint64_t* samples, std::size_t n,
                     std::size_t n_buckets = 64) {
        if (n == 0 || n_buckets == 0) {
            return;
        }
        std::uint64_t lo = samples[0], hi = samples[0];
        for (std::size_t i = 1; i < n; ++i) {
            if (samples[i] < lo) {
                lo = samples[i];
            }
            if (samples[i] > hi) {
                hi = samples[i];
            }
        }
        if (hi == lo) {
            bounds_.push_back(hi);
            cum_.push_back(n);
            total_ = n;
            min_ = lo;
            return;
        }
        bounds_.resize(n_buckets);
        std::vector<std::uint64_t> counts(n_buckets, 0);
        const std::uint64_t span = hi - lo;
        for (std::size_t i = 0; i < n; ++i) {
            // Integer bucketing: b = (s - lo) * n_buckets / span.
            // __uint128_t keeps the multiply exact.
            std::size_t b = static_cast<std::size_t>(
                (static_cast<__uint128_t>(samples[i] - lo) * n_buckets) / span);
            if (b >= n_buckets) {
                b = n_buckets - 1;
            }
            ++counts[b];
        }
        std::uint64_t c = 0;
        for (std::size_t b = 0; b < n_buckets; ++b) {
            bounds_[b] =
                lo + static_cast<std::uint64_t>(
                         (static_cast<__uint128_t>(span) * (b + 1)) / n_buckets);
            c += counts[b];
            cum_.push_back(c);
        }
        total_ = c;
        min_ = lo;
    }

    std::uint64_t sample(std::uint64_t& rng) const noexcept {
        if (total_ == 0) {
            return 0;
        }
        const std::uint64_t r = splitmix64(rng) % total_;
        // First bucket with cum_ > r.
        std::size_t lo = 0, hi = cum_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (cum_[mid] > r) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        const std::size_t b = lo < bounds_.size() ? lo : bounds_.size() - 1;
        const std::uint64_t upper = bounds_[b];
        const std::uint64_t lower = b == 0 ? min_ : bounds_[b - 1];
        if (upper <= lower) {
            return upper;
        }
        return lower + splitmix64(rng) % (upper - lower);
    }

private:
    std::vector<std::uint64_t> bounds_;
    std::vector<std::uint64_t> cum_;
    std::uint64_t total_ = 0;
    std::uint64_t min_ = 0;
};

static_assert(LatencyModel<ConstantLatency>);
static_assert(LatencyModel<JitteredLatency>);
static_assert(LatencyModel<EmpiricalLatency>);

// Feed + order latencies bundled for the simulator template.
template <LatencyModel FeedLat, LatencyModel OrderLat>
struct LatencyPair {
    FeedLat feed;
    OrderLat order;
};

}  // namespace kairos
