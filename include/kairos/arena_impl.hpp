#pragma once

// Arena implementation: template methods for Arena<Bot>.
// This file is included at the end of kairos/arena.hpp.

#include <algorithm>
#include <chrono>

namespace kairos {
namespace arena {

namespace detail {

// Check if a bot callback exceeded the time budget.
inline bool check_budget(
    const std::chrono::steady_clock::time_point& start,
    std::uint64_t budget_ns, std::string& dq_reason) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    if (static_cast<std::uint64_t>(ns) > budget_ns) {
        dq_reason = "callback time budget exceeded";
        return false;
    }
    return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Context methods.
// ---------------------------------------------------------------------------

template <typename Bot>
std::uint64_t Arena<Bot>::Context::now() const {
    return arena_->strategy_now_;
}

template <typename Bot>
const L2Book& Arena<Bot>::Context::book() const {
    return arena_->view_;
}

template <typename Bot>
const Account& Arena<Bot>::Context::account() const {
    return arena_->account_;
}

template <typename Bot>
std::uint64_t Arena<Bot>::Context::send_limit(SimSide side,
                                              std::int64_t price_ticks,
                                              std::int64_t qty_lots,
                                              bool post_only) {
    return arena_->bot_send_limit(side, price_ticks, qty_lots, post_only);
}

template <typename Bot>
void Arena<Bot>::Context::cancel(std::uint64_t order_id) {
    arena_->bot_cancel(order_id);
}

template <typename Bot>
std::uint64_t Arena<Bot>::Context::modify(std::uint64_t order_id,
                                          std::int64_t new_price_ticks,
                                          std::int64_t new_qty_lots) {
    return arena_->bot_modify(order_id, new_price_ticks, new_qty_lots);
}

// ---------------------------------------------------------------------------
// Bot order submission.
// ---------------------------------------------------------------------------

template <typename Bot>
std::uint64_t Arena<Bot>::bot_send_limit(SimSide side, std::int64_t price_ticks,
                                         std::int64_t qty_lots,
                                         bool post_only) {
    // Position limit: reject if this order would breach.
    const std::int64_t pos = account_.position_lots;
    const std::int64_t new_pos =
        (side == SimSide::Bid) ? pos + qty_lots : pos - qty_lots;
    if (std::abs(new_pos) > cfg_.max_position_lots) {
        ++limit_rejects_;  // blocked by position limit (anti-cheat)
        return 0;
    }

    // Rate limit: max orders per second.
    const std::uint64_t now = strategy_now_;
    order_times_.erase(
        std::remove_if(order_times_.begin(), order_times_.end(),
                       [now](std::uint64_t t) { return now - t > 1'000'000'000ULL; }),
        order_times_.end());
    if (order_times_.size() >= cfg_.max_orders_per_sec) {
        ++limit_rejects_;  // blocked by rate limit (anti-cheat)
        return 0;
    }
    order_times_.push_back(now);

    const std::uint64_t id = next_order_id_++;
    PendingOrder po;
    po.effect_time = now + cfg_.order_latency_ns;
    po.id = id;
    po.side = side;
    po.price = price_ticks;
    po.qty = qty_lots;
    po.post_only = post_only;
    pending_.push_back(po);
    return id;
}

template <typename Bot>
void Arena<Bot>::bot_cancel(std::uint64_t order_id) {
    // If still pending (not yet sent), remove it.
    pending_.erase(
        std::remove_if(pending_.begin(), pending_.end(),
                       [order_id](const PendingOrder& po) {
                           return po.id == order_id;
                       }),
        pending_.end());
    // If already in the engine, submit a cancel (with latency).
    // (Simplified: cancels take effect immediately for now.)
    Event evts[4];
    engine_.book().cancel(OrderId(order_id), Timestamp(strategy_now_), evts, 4);
}

template <typename Bot>
std::uint64_t Arena<Bot>::bot_modify(std::uint64_t order_id,
                                     std::int64_t new_price_ticks,
                                     std::int64_t new_qty_lots) {
    // Modify = cancel + new order (loses queue position).
    bot_cancel(order_id);
    // (We don't know the side; assume bid — this is a simplification.
    //  A real implementation would track the side.)
    return bot_send_limit(SimSide::Bid, new_price_ticks, new_qty_lots, true);
}

// ---------------------------------------------------------------------------
// Arena<Bot>::run
// ---------------------------------------------------------------------------

template <typename Bot>
BotResult Arena<Bot>::run(Bot& bot, const std::string& bot_name) {
    BotResult res;
    res.bot_name = bot_name;
    res.seed = cfg_.seed;

    // Initialize account.
    account_.maker_fee_bp = cfg_.maker_fee_bp;
    account_.taker_fee_bp = cfg_.taker_fee_bp;

    // Seed the book with tiny initial liquidity at 10000 ticks (so generators
    // have a mid to reference). Small size so they don't compete with the bot.
    {
        Event evts[64];
        for (int i = 0; i < 3; ++i) {
            const std::uint64_t bid_id = next_order_id_++;
            const std::uint64_t ask_id = next_order_id_++;
            engine_.book().add(OrderId(bid_id), Side::Bid, OrderType::Limit,
                               Price(10000 - i - 1), Quantity(1000),
                               Timestamp(0), 99, evts, 64);
            engine_.book().add(OrderId(ask_id), Side::Ask, OrderType::Limit,
                               Price(10000 + i + 1), Quantity(1000),
                               Timestamp(0), 99, evts, 64);
        }
    }

    // Warmup: run generators for 5 seconds to build the book.
    std::uint64_t now = 0;
    const std::uint64_t warmup_end = 5'000'000'000ULL;
    while (now < warmup_end) {
        std::uint64_t next_t = warmup_end;
        for (auto& gen : generators_) {
            next_t = std::min(next_t, gen->next_time());
        }
        if (next_t > warmup_end) break;
        now = next_t;
        for (auto& gen : generators_) {
            if (gen->next_time() == now) {
                Event evts[64];
                gen->act(engine_, now, evts, 64);
                // (Ignore events during warmup; just build the book.)
            }
        }
    }

    // Main event loop.
    const std::uint64_t end_time = warmup_end + cfg_.duration_ns;
    std::uint64_t next_timer = warmup_end + cfg_.timer_interval_ns;

    // Clear per-run state.
    pending_.clear();
    order_times_.clear();
    limit_rejects_ = 0;
    struct ViewUpdate {
        std::uint64_t delivery_time;
        std::vector<OrderBook::LevelInfo> levels;
    };
    std::vector<ViewUpdate> view_queue;

    // Bot order tracking: id -> (original qty, side)
    struct BotOrder {
        std::uint64_t id;
        bool is_bid;
        std::int64_t qty_original;
    };
    std::vector<BotOrder> bot_orders;

    // Rate limiting: timestamps of recent orders.
    std::vector<std::uint64_t> order_times;

    Context ctx(this);
    bool disqualified = false;

    auto snapshot_view = [&]() {
        OrderBook::LevelInfo lvls[20];
        const std::size_t n =
            engine_.book().snapshot_levels(lvls, 20, 10);
        ViewUpdate vu;
        vu.delivery_time = now + cfg_.feed_latency_ns;
        vu.levels.assign(lvls, lvls + n);
        view_queue.push_back(std::move(vu));
    };

    // Initial view.
    snapshot_view();

    while (now < end_time && !disqualified) {
        // Find next event time.
        std::uint64_t next_t = end_time;
        for (auto& gen : generators_) {
            next_t = std::min(next_t, gen->next_time());
        }
        for (const auto& po : pending_) {
            next_t = std::min(next_t, po.effect_time);
        }
        for (const auto& vu : view_queue) {
            next_t = std::min(next_t, vu.delivery_time);
        }
        if (cfg_.timer_interval_ns > 0) {
            next_t = std::min(next_t, next_timer);
        }
        if (next_t > end_time) break;
        now = next_t;
        strategy_now_ = now;  // (will be overwritten by view delivery time)

        // 1. Generator actions.
        for (auto& gen : generators_) {
            if (gen->next_time() == now) {
                Event evts[64];
                gen->act(engine_, now, evts, 64);
            }
        }

        // 2. Bot pending orders whose effect time has arrived.
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->effect_time <= now) {
                // Submit to engine.
                Event evts[64];
                const Side side = (it->side == SimSide::Bid) ? Side::Bid : Side::Ask;
                const OrderType type =
                    it->post_only ? OrderType::PostOnly : OrderType::Limit;
                auto orr = engine_.book().add(
                    OrderId(it->id), side, type, Price(it->price),
                    Quantity(it->qty), Timestamp(now), 1 /* bot owner */,
                    evts, 64);
                // Check for ack/reject.
                for (std::size_t i = 0; i < orr.events; ++i) {
                    if (evts[i].type == EventType::Ack) {
                        bot_orders.push_back(
                            {it->id, it->side == SimSide::Bid, it->qty});
                        ++res.acks;
                    } else if (evts[i].type == EventType::Reject) {
                        ++res.rejects;
                    }
                }
                // (Fills from taker orders are detected below via polling.)
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }

        // 3. Check bot orders for fills (via get_order polling).
        for (auto it = bot_orders.begin(); it != bot_orders.end();) {
            Order ord;
            if (engine_.book().get_order(OrderId(it->id), ord)) {
                const std::int64_t filled =
                    it->qty_original - ord.remaining.lots;
                if (filled > 0) {
                    // Bot got filled (as maker).
                    SimFill f;
                    f.order_id = it->id;
                    f.side = it->is_bid ? SimSide::Bid : SimSide::Ask;
                    // (Price from the order; actual fill price may differ
                    //  slightly for multi-level fills — simplified.)
                    f.price_ticks = ord.price.ticks;
                    f.qty_lots = filled;
                    f.is_maker = true;
                    // Update account.
                    account_.apply_fill(f.side, f.price_ticks, f.qty_lots,
                                        f.is_maker);
                    ++res.fills;
                    // Deliver callback (with time budget).
                    strategy_now_ = now;
                    auto t0 = std::chrono::steady_clock::now();
                    bot.on_own_fill(ctx, f);
                    if (!detail::check_budget(t0, cfg_.callback_time_budget_ns,
                                              res.dq_reason)) {
                        disqualified = true;
                        res.disqualified = true;
                        break;
                    }
                    // Update tracked qty.
                    it->qty_original = ord.remaining.lots;
                    if (ord.remaining.lots == 0) {
                        it = bot_orders.erase(it);
                        continue;
                    }
                }
                ++it;
            } else {
                // Order not found: fully filled or cancelled.
                // (We don't distinguish here; if it was cancelled by us,
                //  we'd have removed it. Assume filled.)
                it = bot_orders.erase(it);
            }
        }
        if (disqualified) break;

        // 4. Deliver view updates.
        for (auto it = view_queue.begin(); it != view_queue.end();) {
            if (it->delivery_time <= now) {
                // Update the view book from the snapshot.
                view_.clear();
                for (const auto& lvl : it->levels) {
                    view_.upsert(lvl.is_bid, lvl.price_ticks, lvl.qty_lots);
                }
                strategy_now_ = it->delivery_time;
                auto t0 = std::chrono::steady_clock::now();
                bot.on_book_update(ctx);
                if (!detail::check_budget(t0, cfg_.callback_time_budget_ns,
                                          res.dq_reason)) {
                    disqualified = true;
                    res.disqualified = true;
                    break;
                }
                it = view_queue.erase(it);
            } else {
                ++it;
            }
        }
        if (disqualified) break;

        // 5. Bot timer.
        if (cfg_.timer_interval_ns > 0 && now >= next_timer) {
            strategy_now_ = now;
            auto t0 = std::chrono::steady_clock::now();
            bot.on_timer(ctx);
            if (!detail::check_budget(t0, cfg_.callback_time_budget_ns,
                                      res.dq_reason)) {
                disqualified = true;
                res.disqualified = true;
                break;
            }
            next_timer += cfg_.timer_interval_ns;
            // Sample equity/inventory for the report.
            // (Mid from engine book.)
            Price bb(0), ba(0);
            if (engine_.book().best_bid(bb) && engine_.book().best_ask(ba)) {
                const std::int64_t mid = (bb.ticks + ba.ticks) / 2;
                res.equity_curve.emplace_back(now,
                                              account_.equity_lots(mid));
                res.inventory_curve.emplace_back(now,
                                                 account_.position_lots);
            }
        }

        // 6. Schedule next view update if the book changed.
        // (Simplified: always snapshot after actions.)
        snapshot_view();
    }

    res.final_position_lots = account_.position_lots;
    res.realized_pnl_lots = account_.realized_pnl_lots;
    res.rejects += limit_rejects_;  // include anti-cheat rejections
    return res;
}

// ---------------------------------------------------------------------------
// Scoring.
// ---------------------------------------------------------------------------

template <typename Bot>
Score Arena<Bot>::score(const std::vector<BotResult>& results,
                       const ScoringConfig& cfg) {
    Score s;
    if (results.empty()) return s;

    // PnL: mean across seeds (in dollars, assuming lots are scaled).
    double sum_pnl = 0.0;
    double sum_inv = 0.0;
    for (const auto& r : results) {
        // Convert lots to dollars (simplified: assume 1 lot = $1e-6).
        const double pnl_dollars = r.realized_pnl_lots / 1e6;
        sum_pnl += pnl_dollars;
        // Inventory penalty: mean |position| from the curve.
        double mean_abs_pos = 0.0;
        for (const auto& [t, pos] : r.inventory_curve) {
            mean_abs_pos += std::abs(static_cast<double>(pos)) / 1e6;
        }
        if (!r.inventory_curve.empty()) {
            mean_abs_pos /= r.inventory_curve.size();
        }
        sum_inv += mean_abs_pos;
    }
    s.pnl = sum_pnl / results.size();
    s.inventory_penalty = (sum_inv / results.size()) * cfg.spread_cost_per_lot;

    // Risk-adjusted: Sharpe-like (mean / std of per-seed PnL).
    if (results.size() > 1) {
        const double mean = s.pnl;
        double var = 0.0;
        for (const auto& r : results) {
            const double p = r.realized_pnl_lots / 1e6;
            var += (p - mean) * (p - mean);
        }
        var /= (results.size() - 1);
        if (var > 0) {
            s.risk_adjusted = mean / std::sqrt(var);
        }
    }

    s.total = s.pnl - cfg.lambda_inventory * s.inventory_penalty +
              cfg.lambda_risk * s.risk_adjusted;
    return s;
}

}  // namespace arena
}  // namespace kairos
