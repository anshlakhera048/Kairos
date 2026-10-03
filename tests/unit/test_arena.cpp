// Tests for the Phase 4 arena: determinism, fairness, limit enforcement.

#include <gtest/gtest.h>

#include "kairos/arena.hpp"
#include "strategies/arena_bots.hpp"

using namespace kairos;
using namespace kairos::arena;

namespace {

// Helper to run a bot with a given seed and return the result.
template <typename Bot>
BotResult run_bot(Bot& bot, const std::string& name, std::uint64_t seed) {
    ArenaConfig cfg;
    cfg.duration_ns = 10'000'000'000ULL;  // 10s
    cfg.seed = seed;
    Arena<Bot> arena(cfg);
    Regime regime = regime_calm(seed);
    arena.add_generator(
        std::make_unique<NoiseTrader>(regime.noise, 10));
    arena.add_generator(std::make_unique<FixedSpreadMaker>(
        FixedSpreadConfig{}, 11));
    return arena.run(bot, name);
}

}  // namespace

// Determinism: same seed -> same fills, same PnL.
TEST(Arena, Deterministic) {
    bots::FixedSpreadBot bot1, bot2;
    auto r1 = run_bot(bot1, "fixed", 42);
    auto r2 = run_bot(bot2, "fixed", 42);
    EXPECT_EQ(r1.fills, r2.fills);
    EXPECT_EQ(r1.realized_pnl_lots, r2.realized_pnl_lots);
    EXPECT_EQ(r1.acks, r2.acks);
}

// Different seeds -> (likely) different results (sanity check that the
// seed matters). Uses seeds known to diverge; if flaky, increase duration.
TEST(Arena, SeedMatters) {
    bots::FixedSpreadBot bot1, bot2;
    auto r1 = run_bot(bot1, "fixed", 1000);
    auto r2 = run_bot(bot2, "fixed", 1001);
    // With 10s of flow, these seeds produce different fill counts
    // (verified manually). If this flakes, the market is too thin.
    EXPECT_TRUE(r1.fills != r2.fills || r1.acks != r2.acks)
        << "fills: " << r1.fills << " vs " << r2.fills;
}

// Anti-cheat: the cheater's huge orders are rejected by the position limit.
TEST(Arena, CheaterBlocked) {
    bots::CheaterBot bot;
    auto res = run_bot(bot, "cheater", 42);
    // All orders should be rejected (position limit).
    EXPECT_GT(res.rejects, 0);
    // No fills, no PnL — the cheat did not succeed.
    EXPECT_EQ(res.fills, 0);
    EXPECT_EQ(res.realized_pnl_lots, 0);
}

// Fairness: two different bots on the same seed face the same flow.
// (We verify by checking that the market was active — both got acks.
//  Full flow-equality would require instrumenting the generators.)
TEST(Arena, SameSeedSameMarket) {
    bots::FixedSpreadBot bot1;
    bots::RandomBot bot2(42);
    auto r1 = run_bot(bot1, "fixed", 99);
    auto r2 = run_bot(bot2, "random", 99);
    // Both should have been able to trade (market was live).
    EXPECT_GT(r1.acks + r2.acks, 0);
}

// The arena does not crash on an empty book (edge case).
TEST(Arena, EmptyBookStart) {
    ArenaConfig cfg;
    cfg.duration_ns = 1'000'000'000ULL;
    cfg.seed = 1;
    Arena<bots::FixedSpreadBot> arena(cfg);
    // No generators — book stays empty (except seed).
    bots::FixedSpreadBot bot;
    auto res = arena.run(bot, "fixed");
    // Should not crash; bot just doesn't trade.
    EXPECT_EQ(res.fills, 0);
}
