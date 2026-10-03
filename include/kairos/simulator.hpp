#pragma once

// Strategy API and event-driven simulator.
//
// A strategy is a type S providing (all taking Context&):
//   void on_book_update(Context&);
//   void on_trade(Context& ctx, const TradeInfo&);
//   void on_own_fill(Context& ctx, const SimFill&);
//   void on_ack(Context& ctx, const AckInfo&);
//   void on_reject(Context& ctx, std::uint64_t order_id);
//   void on_cancel(Context& ctx, std::uint64_t order_id);
//   void on_timer(Context& ctx);
//
// Compile-time dispatch (templates/concepts): the strategy type is a
// template parameter, so callbacks inline into the simulation loop.
// A virtual interface would cost an indirect branch per event and block
// inlining across the strategy/simulator boundary; here the hot loop is
// market-event processing, and the strategy's logic should be as cheap
// as a direct call.
//
// No-lookahead is structural: the strategy only sees the delayed view
// book and events delivered at time <= now. It cannot access the replay,
// the true (exchange-time) book, or future events — the API does not
// expose them. The cheat test (tests/unit/test_simulator.cpp) verifies
// the latency enforcement mechanism.
//
// Timing model:
//   - Market event at exchange time T is visible to the strategy at
//     T + feed_latency.sample().
//   - A strategy action at time S takes effect at the exchange at
//     S + order_latency.sample().
//   - Fill reports and acks travel exchange -> strategy (feed latency).
//   - The account is updated when the strategy is INFORMED (at fill
//     report delivery), not at exchange time — otherwise the strategy
//     could observe fills early via ctx.account().

#include <cstdint>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "kairos/account.hpp"
#include "kairos/data_format.hpp"
#include "kairos/fill_model.hpp"
#include "kairos/l2_book.hpp"
#include "kairos/latency.hpp"

namespace kairos {

struct TradeInfo {
    std::uint64_t exchange_ts_ns = 0;
    std::int64_t price_ticks = 0;
    std::int64_t qty_lots = 0;
    bool taker_is_bid = false;
};

struct AckInfo {
    std::uint64_t order_id = 0;
    std::uint64_t effective_exchange_ts = 0;  // when it took effect (past)
};

template <typename S, typename Ctx>
concept Strategy = requires(S& s, Ctx& ctx, const TradeInfo& t,
                             const SimFill& f, const AckInfo& a,
                             std::uint64_t id) {
    { s.on_book_update(ctx) } -> std::same_as<void>;
    { s.on_trade(ctx, t) } -> std::same_as<void>;
    { s.on_own_fill(ctx, f) } -> std::same_as<void>;
    { s.on_ack(ctx, a) } -> std::same_as<void>;
    { s.on_reject(ctx, id) } -> std::same_as<void>;
    { s.on_cancel(ctx, id) } -> std::same_as<void>;
    { s.on_timer(ctx) } -> std::same_as<void>;
};

template <typename S, typename FeedLat, typename OrderLat, typename Queue>
class Simulator {
public:
    struct Config {
        FeedLat feed_lat;
        OrderLat order_lat;
        Queue queue;
        std::uint64_t timer_interval_ns = 0;  // 0 = no periodic timers
        std::int64_t maker_fee_bp = 0;
        std::int64_t taker_fee_bp = 0;
        std::uint64_t rng_seed = 0x9e3779b97f4a7c15ULL;
    };

    // Strategy-facing API.
    class Context {
    public:
        // Simulated time as the strategy sees it (delayed by feed latency).
        std::uint64_t now() const { return sim_->strategy_now_; }
        // The delayed book view (never ahead of now()).
        const L2Book& book() const { return sim_->view_book_; }
        // The account as the strategy knows it (updated on fill reports).
        const Account& account() const { return sim_->account_; }

        // Send a limit order. Returns the order ID synchronously; the
        // order takes effect at the exchange at now() + order latency.
        std::uint64_t send_limit(SimSide side, std::int64_t price_ticks,
                                 std::int64_t qty_lots,
                                 bool post_only = true) {
            return sim_->send_limit(side, price_ticks, qty_lots, post_only);
        }
        // Cancel an order (takes effect after order latency; races with
        // fills on exchange time).
        void cancel(std::uint64_t order_id) {
            sim_->schedule_cancel(order_id);
        }
        // Modify = cancel + replace (loses queue position). Returns the
        // new order ID.
        std::uint64_t modify(std::uint64_t order_id,
                             std::int64_t new_price_ticks,
                             std::int64_t new_qty_lots) {
            return sim_->modify(order_id, new_price_ticks, new_qty_lots);
        }

    private:
        friend class Simulator;
        explicit Context(Simulator* sim) : sim_(sim) {}
        Simulator* sim_;
    };

    struct Result {
        Account account;
        std::uint64_t market_events = 0;
        std::uint64_t fills = 0;
        std::uint64_t acks = 0;
        std::uint64_t rejects = 0;
        std::uint64_t cancels = 0;
    };

    explicit Simulator(const Config& cfg) : cfg_(cfg), rng_(cfg.rng_seed) {
        account_.maker_fee_bp = cfg.maker_fee_bp;
        account_.taker_fee_bp = cfg.taker_fee_bp;
    }

    Result run(const std::vector<std::string>& paths, S& strategy) {
        static_assert(Strategy<S, Context>,
                      "Strategy must provide the full callback interface");
        data::CaptureReader reader(paths);
        data::EventView ev;
        bool has_ev = reader.next(ev);
        Context ctx(this);
        Result res;

        if (cfg_.timer_interval_ns > 0) {
            schedule_delivery(cfg_.timer_interval_ns,
                              DeliveryKind::Timer);
        }

        while (has_ev || !deliveries_.empty() || !effects_.empty()) {
            if (deliveries_.empty()) {
                // No strategy-visible work pending; advance the exchange
                // to generate some.
                if (!advance_exchange(has_ev, ev, reader, res)) {
                    break;
                }
                continue;
            }
            const std::uint64_t next_del = deliveries_.top().at;
            // Bring the exchange up to the next delivery time, then
            // deliver in time order.
            process_exchange_up_to(next_del, has_ev, ev, reader, res);
            Delivery d = deliveries_.top();
            deliveries_.pop();
            strategy_now_ = d.at;
            dispatch(d, strategy, ctx, res);
            // The timer only reschedules while market data remains: it
            // wakes the strategy DURING the simulation. Without this
            // guard the timer would keep the delivery queue non-empty
            // forever after the data ends.
            if (d.kind == DeliveryKind::Timer &&
                cfg_.timer_interval_ns > 0 && has_ev) {
                schedule_delivery(d.at + cfg_.timer_interval_ns,
                                  DeliveryKind::Timer);
            }
        }
        res.account = account_;
        return res;
    }

private:
    enum class DeliveryKind : std::uint8_t {
        Market,
        Fill,
        Ack,
        Reject,
        Cancel,
        Timer
    };
    struct Delivery {
        std::uint64_t at = 0;   // deliver time (strategy time)
        std::uint64_t seq = 0;  // tie-breaker
        DeliveryKind kind = DeliveryKind::Timer;
        data::EventView ev;      // Market: view into the mmap'd capture
        SimFill fill;            // Fill
        AckInfo ack;             // Ack
        std::uint64_t order_id = 0;  // Reject, Cancel
    };
    struct DeliveryCmp {
        bool operator()(const Delivery& a, const Delivery& b) const {
            if (a.at != b.at) {
                return a.at > b.at;  // min-heap
            }
            return a.seq > b.seq;
        }
    };

    enum class EffectKind : std::uint8_t { Place, Cancel };
    struct OrderEffect {
        std::uint64_t at = 0;  // exchange effective time
        std::uint64_t seq = 0;
        EffectKind kind = EffectKind::Place;
        std::uint64_t order_id = 0;
    };
    struct EffectCmp {
        bool operator()(const OrderEffect& a, const OrderEffect& b) const {
            if (a.at != b.at) {
                return a.at > b.at;
            }
            return a.seq > b.seq;
        }
    };

    struct PendingOrder {
        std::uint64_t id = 0;
        SimSide side = SimSide::Bid;
        std::int64_t price_ticks = 0;
        std::int64_t qty_lots = 0;
        bool post_only = true;
    };

    Config cfg_;
    std::uint64_t rng_;
    std::uint64_t next_seq_ = 0;
    std::uint64_t next_order_id_ = 1;
    std::uint64_t strategy_now_ = 0;
    std::uint64_t exchange_now_ = 0;

    L2Book true_book_;  // exchange-time book (queue model + fills)
    L2Book view_book_;  // strategy-time book (delayed)
    Account account_;    // strategy's view (updated on fill reports)

    std::priority_queue<Delivery, std::vector<Delivery>, DeliveryCmp>
        deliveries_;
    std::priority_queue<OrderEffect, std::vector<OrderEffect>, EffectCmp>
        effects_;
    std::unordered_map<std::uint64_t, PendingOrder> pending_;
    std::unordered_map<std::uint64_t, TrackedOrder> active_;

    void schedule_delivery(std::uint64_t at, DeliveryKind kind) {
        Delivery d;
        d.at = at;
        d.seq = next_seq_++;
        d.kind = kind;
        deliveries_.push(d);
    }

    void schedule_fill_delivery(std::uint64_t at, const SimFill& f) {
        Delivery d;
        d.at = at;
        d.seq = next_seq_++;
        d.kind = DeliveryKind::Fill;
        d.fill = f;
        deliveries_.push(d);
    }

    void schedule_ack_delivery(std::uint64_t at, const AckInfo& a) {
        Delivery d;
        d.at = at;
        d.seq = next_seq_++;
        d.kind = DeliveryKind::Ack;
        d.ack = a;
        deliveries_.push(d);
    }

    void schedule_id_delivery(std::uint64_t at, DeliveryKind kind,
                              std::uint64_t order_id) {
        Delivery d;
        d.at = at;
        d.seq = next_seq_++;
        d.kind = kind;
        d.order_id = order_id;
        deliveries_.push(d);
    }

    std::uint64_t send_limit(SimSide side, std::int64_t price_ticks,
                             std::int64_t qty_lots, bool post_only) {
        const std::uint64_t id = next_order_id_++;
        PendingOrder p;
        p.id = id;
        p.side = side;
        p.price_ticks = price_ticks;
        p.qty_lots = qty_lots;
        p.post_only = post_only;
        pending_[id] = p;
        OrderEffect e;
        // Effect time >= strategy_now_ >= exchange_now_ (no backdating).
        e.at = strategy_now_ + cfg_.order_lat.sample(rng_);
        e.seq = next_seq_++;
        e.kind = EffectKind::Place;
        e.order_id = id;
        effects_.push(e);
        return id;
    }

    void schedule_cancel(std::uint64_t order_id) {
        OrderEffect e;
        e.at = strategy_now_ + cfg_.order_lat.sample(rng_);
        e.seq = next_seq_++;
        e.kind = EffectKind::Cancel;
        e.order_id = order_id;
        effects_.push(e);
    }

    std::uint64_t modify(std::uint64_t order_id,
                         std::int64_t new_price_ticks,
                         std::int64_t new_qty_lots) {
        // Cancel the old (it may already be gone; harmless), place new.
        // Queue position is lost, matching exchange semantics.
        SimSide side = SimSide::Bid;
        bool post_only = true;
        auto pit = pending_.find(order_id);
        if (pit != pending_.end()) {
            side = pit->second.side;
            post_only = pit->second.post_only;
        } else {
            auto ait = active_.find(order_id);
            if (ait != active_.end()) {
                side = ait->second.side;
            }
        }
        schedule_cancel(order_id);
        return send_limit(side, new_price_ticks, new_qty_lots, post_only);
    }

    // Advance the exchange timeline by one event (market or order effect).
    // Returns false when no exchange work remains.
    bool advance_exchange(bool& has_ev, data::EventView& ev,
                          data::CaptureReader& reader, Result& res) {
        const std::uint64_t INF = ~0ULL;
        const std::uint64_t next_mkt =
            has_ev ? ev.header->exchange_ts_ns : INF;
        const std::uint64_t next_eff =
            effects_.empty() ? INF : effects_.top().at;
        if (next_mkt == INF && next_eff == INF) {
            return false;
        }
        // On ties, order effects win (a cancel at the same instant as a
        // trade cancels before the trade can fill it).
        if (next_eff <= next_mkt) {
            OrderEffect e = effects_.top();
            effects_.pop();
            exchange_now_ = e.at;
            apply_order_effect(e, res);
        } else {
            exchange_now_ = ev.header->exchange_ts_ns;
            apply_market_event(ev, res);
            has_ev = reader.next(ev);
        }
        return true;
    }

    void process_exchange_up_to(std::uint64_t D, bool& has_ev,
                                data::EventView& ev,
                                data::CaptureReader& reader, Result& res) {
        const std::uint64_t INF = ~0ULL;
        for (;;) {
            const std::uint64_t next_mkt =
                has_ev ? ev.header->exchange_ts_ns : INF;
            const std::uint64_t next_eff =
                effects_.empty() ? INF : effects_.top().at;
            std::uint64_t nxt =
                next_mkt < next_eff ? next_mkt : next_eff;
            if (nxt > D) {
                break;
            }
            if (!advance_exchange(has_ev, ev, reader, res)) {
                break;
            }
        }
    }

    void apply_market_event(const data::EventView& ev, Result& res) {
        const std::uint64_t T = ev.header->exchange_ts_ns;
        const auto type = static_cast<data::EventType>(ev.header->type);
        true_book_.apply(ev);
        ++res.market_events;

        if (type == data::EventType::Trade) {
            const auto& l = ev.levels[0];
            const bool taker_is_bid = (l.side == 0);
            std::vector<std::uint64_t> done;
            for (auto& kv : active_) {
                TrackedOrder& o = kv.second;
                const bool i_am_bid = (o.side == SimSide::Bid);
                std::int64_t filled = 0;
                if (l.price_ticks == o.price_ticks &&
                    i_am_bid != taker_is_bid) {
                    // Trade at my price, taking from my side of the book.
                    filled = cfg_.queue.on_trade(o, l.qty_lots);
                } else if (i_am_bid && l.price_ticks < o.price_ticks) {
                    // Seller-initiated trade below my bid: my higher bid
                    // would have been filled first.
                    filled = cfg_.queue.on_trade_through(o);
                } else if (!i_am_bid && l.price_ticks > o.price_ticks) {
                    filled = cfg_.queue.on_trade_through(o);
                }
                if (filled > 0) {
                    // Account updated at REPORT time, not here (no leak).
                    SimFill f;
                    f.order_id = o.id;
                    f.side = o.side;
                    f.price_ticks = o.price_ticks;
                    f.qty_lots = filled;
                    f.is_maker = true;
                    schedule_fill_delivery(
                        T + cfg_.feed_lat.sample(rng_), f);
                    ++res.fills;
                    if (o.remaining == 0) {
                        done.push_back(o.id);
                    }
                }
            }
            for (auto id : done) {
                active_.erase(id);
            }
        } else {
            // Snapshot/diff: refresh queue states. The level qty passed is
            // the HISTORICAL qty (my ghost orders are not in the book).
            for (auto& kv : active_) {
                TrackedOrder& o = kv.second;
                const bool is_bid = (o.side == SimSide::Bid);
                cfg_.queue.on_level_update(
                    o, true_book_.level_qty(is_bid, o.price_ticks));
            }
        }

        // The strategy sees this at T + feed latency. The EventView points
        // into the mmap'd capture, so no copy is needed.
        Delivery d;
        d.at = T + cfg_.feed_lat.sample(rng_);
        d.seq = next_seq_++;
        d.kind = DeliveryKind::Market;
        d.ev = ev;
        deliveries_.push(d);
    }

    void apply_order_effect(const OrderEffect& e, Result& res) {
        if (e.kind == EffectKind::Place) {
            auto it = pending_.find(e.order_id);
            if (it == pending_.end()) {
                return;  // cancelled before it took effect
            }
            PendingOrder p = it->second;
            pending_.erase(it);

            L2Level best_ask, best_bid;
            const bool has_ask = true_book_.best_ask(best_ask);
            const bool has_bid = true_book_.best_bid(best_bid);
            const bool crosses =
                (p.side == SimSide::Bid && has_ask &&
                 p.price_ticks >= best_ask.price_ticks) ||
                (p.side == SimSide::Ask && has_bid &&
                 p.price_ticks <= best_bid.price_ticks);

            if (crosses) {
                if (p.post_only) {
                    schedule_id_delivery(
                        e.at + cfg_.feed_lat.sample(rng_),
                        DeliveryKind::Reject, p.id);
                    ++res.rejects;
                } else {
                    taker_fill(p, e.at, res);
                }
                return;
            }
            TrackedOrder t;
            t.id = p.id;
            t.side = p.side;
            t.price_ticks = p.price_ticks;
            t.qty_lots = p.qty_lots;
            t.remaining = p.qty_lots;
            const bool is_bid = (p.side == SimSide::Bid);
            cfg_.queue.on_place(
                t, true_book_.level_qty(is_bid, p.price_ticks));
            active_[p.id] = t;
            AckInfo a;
            a.order_id = p.id;
            a.effective_exchange_ts = e.at;
            schedule_ack_delivery(e.at + cfg_.feed_lat.sample(rng_), a);
            ++res.acks;
        } else {
            // Cancel: check active, then pending.
            auto ait = active_.find(e.order_id);
            if (ait != active_.end()) {
                active_.erase(ait);
                schedule_id_delivery(
                    e.at + cfg_.feed_lat.sample(rng_),
                    DeliveryKind::Cancel, e.order_id);
                ++res.cancels;
                return;
            }
            auto pit = pending_.find(e.order_id);
            if (pit != pending_.end()) {
                pending_.erase(pit);
                schedule_id_delivery(
                    e.at + cfg_.feed_lat.sample(rng_),
                    DeliveryKind::Cancel, e.order_id);
                ++res.cancels;
            }
            // Else: already filled or unknown; ignore.
        }
    }

    // A marketable limit order takes liquidity without mutating the
    // historical book (assumption: my orders don't move the market).
    // One fill report per level consumed.
    void taker_fill(const PendingOrder& p, std::uint64_t E, Result& res) {
        std::int64_t remaining = p.qty_lots;
        const bool i_am_bid = (p.side == SimSide::Bid);
        const auto& levels =
            i_am_bid ? true_book_.ask_levels() : true_book_.bid_levels();
        std::int64_t total_filled = 0;
        for (const auto& lvl : levels) {
            if (remaining <= 0) {
                break;
            }
            const bool acceptable = i_am_bid
                                        ? (lvl.price_ticks <= p.price_ticks)
                                        : (lvl.price_ticks >= p.price_ticks);
            if (!acceptable) {
                break;
            }
            const std::int64_t take =
                remaining < lvl.qty_lots ? remaining : lvl.qty_lots;
            SimFill f;
            f.order_id = p.id;
            f.side = p.side;
            f.price_ticks = lvl.price_ticks;
            f.qty_lots = take;
            f.is_maker = false;
            // Account updated at report time.
            schedule_fill_delivery(E + cfg_.feed_lat.sample(rng_), f);
            ++res.fills;
            remaining -= take;
            total_filled += take;
        }
        // Unfilled remainder (thin book) rests at the limit price.
        if (remaining > 0) {
            TrackedOrder t;
            t.id = p.id;
            t.side = p.side;
            t.price_ticks = p.price_ticks;
            t.qty_lots = p.qty_lots;
            t.remaining = remaining;
            cfg_.queue.on_place(
                t, true_book_.level_qty(i_am_bid, p.price_ticks));
            active_[p.id] = t;
        }
        // The order took effect (fully or partially as a taker): ack it
        // so the strategy knows the exchange timestamp.
        AckInfo a;
        a.order_id = p.id;
        a.effective_exchange_ts = E;
        schedule_ack_delivery(E + cfg_.feed_lat.sample(rng_), a);
        ++res.acks;
    }

    void dispatch(const Delivery& d, S& strategy, Context& ctx, Result& res) {
        (void)res;
        switch (d.kind) {
            case DeliveryKind::Market: {
                view_book_.apply(d.ev);
                const auto type =
                    static_cast<data::EventType>(d.ev.header->type);
                if (type == data::EventType::Trade) {
                    const auto& l = d.ev.levels[0];
                    TradeInfo t;
                    t.exchange_ts_ns = d.ev.header->exchange_ts_ns;
                    t.price_ticks = l.price_ticks;
                    t.qty_lots = l.qty_lots;
                    t.taker_is_bid = (l.side == 0);
                    strategy.on_trade(ctx, t);
                } else {
                    strategy.on_book_update(ctx);
                }
                break;
            }
            case DeliveryKind::Fill:
                // The strategy learns the fill NOW: update its account
                // view first, then inform it.
                account_.apply_fill(d.fill.side, d.fill.price_ticks,
                                    d.fill.qty_lots, d.fill.is_maker);
                strategy.on_own_fill(ctx, d.fill);
                break;
            case DeliveryKind::Ack:
                strategy.on_ack(ctx, d.ack);
                break;
            case DeliveryKind::Reject:
                strategy.on_reject(ctx, d.order_id);
                break;
            case DeliveryKind::Cancel:
                strategy.on_cancel(ctx, d.order_id);
                break;
            case DeliveryKind::Timer:
                strategy.on_timer(ctx);
                break;
        }
    }
};  // class Simulator

}  // namespace kairos
