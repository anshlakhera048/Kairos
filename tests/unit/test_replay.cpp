// Tests for the Phase 2 replay stack: binary format reader, L2 book
// reconstruction, and replay determinism. Uses tests/data/golden.kai,
// a hand-verified capture file (see tools/recorder/format.py).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kairos/data_format.hpp"
#include "kairos/l2_book.hpp"
#include "kairos/replay.hpp"

namespace kairos {
namespace {

const char* kGolden = TEST_DATA_DIR "/golden.kai";

TEST(DataFormat, ReadsGoldenFile) {
    data::MappedFile f(kGolden);
    ASSERT_TRUE(f.valid());
    const data::FileHeader* h = f.header();
    EXPECT_EQ(h->magic, data::kMagic);
    EXPECT_EQ(h->version, data::kVersion);
    EXPECT_EQ(std::string(h->symbol, strnlen(h->symbol, 16)), "TEST-USD");
    EXPECT_EQ(h->price_scale, 100);
    EXPECT_NE(h->crc32, 0u);
    EXPECT_EQ(f.compute_crc32(), h->crc32);
}

TEST(DataFormat, IteratesEvents) {
    data::MappedFile f(kGolden);
    auto it = f.iterate();
    data::EventView ev;
    int n = 0;
    while (it.next(ev)) {
        ++n;
    }
    EXPECT_EQ(n, 4);
}

TEST(DataFormat, RejectsBadFile) {
    data::MappedFile f("/nonexistent/path.kai");
    EXPECT_FALSE(f.valid());
}

TEST(L2Book, ReconstructsGoldenFile) {
    data::MappedFile f(kGolden);
    auto it = f.iterate();
    L2Book book;
    data::EventView ev;
    while (it.next(ev)) {
        book.apply(ev);
        EXPECT_TRUE(book.check_invariants());
    }
    // After all events:
    // snapshot: bids 99900x1000, 99800x500; asks 100100x800, 100200x300
    //   (price_scale=100, qty_scale=1e8: 999.00 -> 99900 ticks, 10.0 -> 1e9 lots)
    // diff 1001: bid 99900 -> 700 lots-units... (7.0 * 1e8), ask 100300 added, bid 99800 removed
    // diff 1003: ask 100100 removed
    L2Level b, a;
    ASSERT_TRUE(book.best_bid(b));
    EXPECT_EQ(b.price_ticks, 99900);
    EXPECT_EQ(b.qty_lots, 700000000LL);  // 7.0 * 1e8
    ASSERT_TRUE(book.best_ask(a));
    EXPECT_EQ(a.price_ticks, 100200);  // 100100 was removed
    EXPECT_EQ(a.qty_lots, 300000000LL);
    EXPECT_EQ(book.bid_count(), 1u);  // 99800 removed
    EXPECT_EQ(book.ask_count(), 2u);  // 100200, 100300
}

struct CountingConsumer {
    int snapshots = 0;
    int diffs = 0;
    int trades = 0;
    std::uint64_t last_trade_price = 0;
    void on_snapshot(std::uint64_t, std::uint64_t) { ++snapshots; }
    void on_diff(std::uint64_t, std::uint64_t) { ++diffs; }
    void on_trade(std::uint64_t, std::uint64_t, std::int64_t p, std::int64_t, bool) {
        ++trades;
        last_trade_price = static_cast<std::uint64_t>(p);
    }
};

TEST(Replay, DeterministicChecksum) {
    ReplayEngine e1, e2;
    CountingConsumer c1, c2;
    const std::vector<std::string> paths = {kGolden};
    const auto s1 = e1.run(paths, c1);
    const auto s2 = e2.run(paths, c2);
    EXPECT_EQ(s1.checksum, s2.checksum);
    EXPECT_NE(s1.checksum, 0u);
    EXPECT_EQ(s1.snapshots, 1);
    EXPECT_EQ(s1.diffs, 2);
    EXPECT_EQ(s1.trades, 1);
    EXPECT_EQ(c1.last_trade_price, 100100u);  // 1001.00 * 100
    // Final book state is identical across runs.
    L2Level b1, b2;
    ASSERT_TRUE(e1.book().best_bid(b1));
    ASSERT_TRUE(e2.book().best_bid(b2));
    EXPECT_EQ(b1.price_ticks, b2.price_ticks);
}

TEST(Replay, LiveCaptureFile) {
    // The live REST-poll capture from 2A testing, if present. Skipped when
    // absent (it is not committed to the repo).
    const char* path = "/tmp/cap_test/coinbase_BTC-USD_1791025123876019006.kai";
    data::MappedFile f(path);
    if (!f.valid()) {
        GTEST_SKIP() << "live capture not present";
    }
    ReplayEngine e;
    CountingConsumer c;
    const auto s = e.run({path}, c);
    EXPECT_GT(s.snapshots, 0u);
    EXPECT_TRUE(e.book().check_invariants());
    L2Level b, a;
    if (e.book().best_bid(b) && e.book().best_ask(a)) {
        EXPECT_LT(b.price_ticks, a.price_ticks);
    }
}

}  // namespace
}  // namespace kairos
