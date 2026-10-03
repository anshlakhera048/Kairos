// Realism-gap experiment driver.
//
// Runs the Avellaneda-Stoikov strategy on the same data under four
// realism levels:
//   L1: fill at touch (NaiveQueue), zero latency
//   L2: fill at touch (NaiveQueue), realistic latency
//   L3: realistic latency + RiskAverseQueue
//   L4: realistic latency + ProbabilisticQueue
//
// Outputs one JSON object per run to stdout (JSON lines), with the
// equity curve and fill log for post-processing.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "kairos/simulator.hpp"
#include "strategies/avellaneda_stoikov.hpp"

namespace kairos {
namespace {

struct FillRecord {
    std::uint64_t t_ns;
    std::int64_t price_ticks;
    std::int64_t qty_lots;
    bool is_bid;  // my side
};

struct EquityPoint {
    std::uint64_t t_ns;
    std::int64_t equity_lots;
};

// Wrapper that logs fills and equity on top of ASStrategy.
class LoggingStrategy {
public:
    explicit LoggingStrategy(const ASStrategyConfig& cfg) : inner_(cfg) {}

    template <typename Ctx>
    void on_book_update(Ctx& ctx) {
        inner_.on_book_update(ctx);
    }
    template <typename Ctx>
    void on_trade(Ctx& ctx, const TradeInfo& t) {
        inner_.on_trade(ctx, t);
    }
    template <typename Ctx>
    void on_own_fill(Ctx& ctx, const SimFill& f) {
        FillRecord r;
        r.t_ns = ctx.now();
        r.price_ticks = f.price_ticks;
        r.qty_lots = f.qty_lots;
        r.is_bid = (f.side == SimSide::Bid);
        fills_.push_back(r);
        inner_.on_own_fill(ctx, f);
    }
    template <typename Ctx>
    void on_ack(Ctx& ctx, const AckInfo& a) {
        inner_.on_ack(ctx, a);
    }
    template <typename Ctx>
    void on_reject(Ctx& ctx, std::uint64_t id) {
        inner_.on_reject(ctx, id);
    }
    template <typename Ctx>
    void on_cancel(Ctx& ctx, std::uint64_t id) {
        inner_.on_cancel(ctx, id);
    }
    template <typename Ctx>
    void on_timer(Ctx& ctx) {
        inner_.on_timer(ctx);
        // Sample equity on timer (mark at mid).
        L2Level bb, ba;
        if (ctx.book().best_bid(bb) && ctx.book().best_ask(ba)) {
            const std::int64_t mid = bb.price_ticks +
                                     (ba.price_ticks - bb.price_ticks) / 2;
            equity_.push_back(
                {ctx.now(), ctx.account().equity_lots(mid)});
        }
    }

    const std::vector<FillRecord>& fills() const { return fills_; }
    const std::vector<EquityPoint>& equity() const { return equity_; }

private:
    ASStrategy inner_;
    std::vector<FillRecord> fills_;
    std::vector<EquityPoint> equity_;
};

void print_json(const char* name, const std::vector<FillRecord>& fills,
                const std::vector<EquityPoint>& equity,
                const Account& acct, std::uint64_t market_events,
                std::uint64_t acks) {
    std::printf("{\"run\": \"%s\", \"market_events\": %llu, \"acks\": %llu,",
                name, (unsigned long long)market_events,
                (unsigned long long)acks);
    std::printf("\"final_position\": %lld, \"realized_pnl\": %lld,",
                (long long)acct.position_lots,
                (long long)acct.realized_pnl_lots);
    std::printf("\"fills\": [");
    for (size_t i = 0; i < fills.size(); ++i) {
        if (i) {
            std::printf(",");
        }
        std::printf("{\"t\": %llu, \"p\": %lld, \"q\": %lld, \"bid\": %s}",
                    (unsigned long long)fills[i].t_ns,
                    (long long)fills[i].price_ticks,
                    (long long)fills[i].qty_lots,
                    fills[i].is_bid ? "true" : "false");
    }
    std::printf("], \"equity\": [");
    for (size_t i = 0; i < equity.size(); ++i) {
        if (i) {
            std::printf(",");
        }
        std::printf("{\"t\": %llu, \"e\": %lld}",
                    (unsigned long long)equity[i].t_ns,
                    (long long)equity[i].equity_lots);
    }
    std::printf("]}\n");
}

template <typename Queue>
void run_level(const char* name, const std::string& path,
               std::uint64_t feed_lat_ns, std::uint64_t order_lat_ns) {
    using Sim = Simulator<LoggingStrategy, ConstantLatency, ConstantLatency,
                          Queue>;
    ASStrategyConfig scfg;
    scfg.model.gamma = 1500.0;
    scfg.model.sigma = 0.0002;
    scfg.model.kappa = 2.0;
    scfg.model.horizon_s = 3600.0;
    scfg.order_size_lots = 1000000;
    scfg.max_inventory_lots = 10000000;
    scfg.max_spread_ticks = 100;

    typename Sim::Config cfg;
    cfg.feed_lat = ConstantLatency{feed_lat_ns};
    cfg.order_lat = ConstantLatency{order_lat_ns};
    cfg.timer_interval_ns = 500000000;  // 500ms
    cfg.maker_fee_bp = -10;
    cfg.taker_fee_bp = 30;

    Sim sim(cfg);
    LoggingStrategy strat(scfg);
    const auto res = sim.run({path}, strat);
    print_json(name, strat.fills(), strat.equity(), res.account,
               res.market_events, res.acks);
}

}  // namespace
}  // namespace kairos

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <capture.kai>\n", argv[0]);
        return 1;
    }
    const std::string path = argv[1];
    // L1: naive fill, zero latency.
    kairos::run_level<kairos::NaiveQueue>("L1_naive", path, 0, 0);
    // L2: naive fill + latency.
    kairos::run_level<kairos::NaiveQueue>("L2_latency", path, 5000000,
                                          2000000);
    // L3: latency + risk-averse queue.
    kairos::run_level<kairos::RiskAverseQueue>("L3_risk_averse", path,
                                               5000000, 2000000);
    // L4: latency + probabilistic queue.
    kairos::run_level<kairos::ProbabilisticQueue>("L4_probabilistic", path,
                                                  5000000, 2000000);
    return 0;
}
