// Naive reference implementation. Mirrors OrderBook semantics exactly;
// written independently against the documented contract.

#include "reference.hpp"

#include <algorithm>

namespace kairos {
namespace reference {

RefBook::OpResult RefBook::add(OrderId id, Side side, OrderType type, Price price,
                               Quantity qty, Timestamp ts, std::uint32_t owner,
                               Event* out, std::size_t out_capacity) noexcept {
    if (!id.valid() || qty.lots <= 0 || out == nullptr || out_capacity == 0) {
        return {Error::InvalidArgs, 0};
    }
    if (by_id_.count(id.id) != 0u) {
        return {Error::DuplicateId, 0};
    }
    if (live_ >= cfg_.max_orders) {
        return {Error::NoCapacity, 0};
    }
    const bool is_market = (type == OrderType::Market);
    if (!is_market && !in_band(price)) {
        return {Error::OutOfBandPrice, 0};
    }
    if (out_capacity < live_ + 1) {
        return {Error::EventBufferFull, 0};
    }

    if (type == OrderType::PostOnly) {
        auto& opp = (side == Side::Bid) ? asks_ : bids_;
        bool crosses = false;
        if (!opp.empty()) {
            const std::int64_t touch =
                (side == Side::Bid) ? opp.begin()->first : opp.rbegin()->first;
            crosses = (side == Side::Bid) ? price.ticks >= touch
                                          : price.ticks <= touch;
        }
        if (crosses) {
            std::size_t n = 0;
            emit(EventType::Reject, id, ts, price, qty, out, n);
            return {Error::WouldCrossPostOnly, n};
        }
    }

    if (type == OrderType::Fok &&
        fillable(side, price, static_cast<std::uint64_t>(qty.lots), owner) <
            static_cast<std::uint64_t>(qty.lots)) {
        std::size_t n = 0;
        emit(EventType::Reject, id, ts, price, qty, out, n);
        return {Error::InsufficientQtyFok, n};
    }

    std::size_t n = 0;
    const Error err = execute_add(id, side, type, price,
                                  static_cast<std::uint64_t>(qty.lots), ts,
                                  owner, out, n);
    return {err, n};
}

std::uint64_t RefBook::fillable(Side taker_side, Price limit, std::uint64_t need,
                                std::uint32_t owner) const {
    std::uint64_t fillable = 0;
    const bool taker_is_bid = (taker_side == Side::Bid);
    const auto& opp = taker_is_bid ? asks_ : bids_;
    if (taker_is_bid) {
        for (auto it = opp.begin();
             it != opp.end() && fillable < need; ++it) {
            if (it->first > limit.ticks) {
                break;
            }
            for (const auto& r : it->second) {
                if (fillable >= need) {
                    break;
                }
                const bool self = (owner != 0 && r.owner == owner &&
                                   cfg_.self_trade != SelfTradePolicy::Allow);
                if (self &&
                    cfg_.self_trade == SelfTradePolicy::CancelIncoming) {
                    return fillable < need ? fillable : need;
                }
                if (!self) {
                    fillable += static_cast<std::uint64_t>(r.qty.lots);
                }
            }
        }
    } else {
        for (auto it = opp.rbegin();
             it != opp.rend() && fillable < need; ++it) {
            if (it->first < limit.ticks) {
                break;
            }
            for (const auto& r : it->second) {
                if (fillable >= need) {
                    break;
                }
                const bool self = (owner != 0 && r.owner == owner &&
                                   cfg_.self_trade != SelfTradePolicy::Allow);
                if (self &&
                    cfg_.self_trade == SelfTradePolicy::CancelIncoming) {
                    return fillable < need ? fillable : need;
                }
                if (!self) {
                    fillable += static_cast<std::uint64_t>(r.qty.lots);
                }
            }
        }
    }
    return fillable < need ? fillable : need;
}

Error RefBook::execute_add(OrderId id, Side side, OrderType type, Price price,
                           std::uint64_t qty_lots, Timestamp ts,
                           std::uint32_t owner, Event* out, std::size_t& n) {
    std::uint64_t remaining = qty_lots;
    const bool taker_is_bid = (side == Side::Bid);
    const bool is_market = (type == OrderType::Market);
    bool rejected = false;

    auto& opp = taker_is_bid ? asks_ : bids_;
    while (remaining > 0 && !rejected && !opp.empty()) {
        // Best opposite level: lowest ask for a bid taker, highest bid for an
        // ask taker.
        auto lit = taker_is_bid ? opp.begin() : std::prev(opp.end());
        if (!is_market) {
            if (taker_is_bid && lit->first > price.ticks) {
                break;
            }
            if (!taker_is_bid && lit->first < price.ticks) {
                break;
            }
        }
        auto& queue = lit->second;
        while (remaining > 0 && !queue.empty()) {
            auto qit = queue.begin();
            RefOrder& r = *qit;
            const bool self = (owner != 0 && r.owner == owner &&
                               cfg_.self_trade != SelfTradePolicy::Allow);
            if (self && cfg_.self_trade == SelfTradePolicy::CancelIncoming) {
                emit(EventType::Reject, id, ts, price,
                     Quantity{static_cast<std::int64_t>(remaining)}, out, n);
                rejected = true;
                remaining = 0;
                break;
            }
            if (self) {  // CancelResting
                const OrderId rid = r.id;
                const Price rp = r.price;
                const Quantity rq = r.qty;
                by_id_.erase(rid.id);
                queue.erase(qit);
                --live_;
                emit(EventType::Cancel, rid, ts, rp, rq, out, n);
                continue;
            }
            const std::uint64_t fill = std::min(
                remaining, static_cast<std::uint64_t>(r.qty.lots));
            r.qty.lots -= static_cast<std::int64_t>(fill);
            remaining -= fill;
            emit(EventType::Fill, id, ts, r.price,
                 Quantity{static_cast<std::int64_t>(fill)}, out, n);
            if (r.qty.lots == 0) {
                by_id_.erase(r.id.id);
                queue.erase(qit);
                --live_;
            }
        }
        if (queue.empty()) {
            // Erase the (now empty) level. Recompute iterator safely.
            if (taker_is_bid) {
                opp.erase(opp.begin());
            } else {
                opp.erase(std::prev(opp.end()));
            }
        }
    }

    const bool can_rest =
        (type == OrderType::Limit || type == OrderType::PostOnly);
    const bool rests = (!rejected && can_rest && remaining > 0);
    if (rests) {
        RefOrder o;
        o.id = id;
        o.price = price;
        o.qty = Quantity{static_cast<std::int64_t>(remaining)};
        o.seq = next_order_seq_++;
        o.owner = owner;
        o.side = side;
        o.type = type;
        auto& q = book(side)[price.ticks];
        q.push_back(o);
        auto it = std::prev(q.end());
        by_id_[id.id] = Loc{side, price.ticks, it};
        ++live_;
    }

    if (!rejected) {
        const std::int64_t resting_qty =
            rests ? static_cast<std::int64_t>(remaining) : 0;
        emit(EventType::Ack, id, ts, price, Quantity{resting_qty}, out, n);
    }
    return rejected ? Error::SelfTradeReject : Error::Ok;
}

RefBook::OpResult RefBook::cancel(OrderId id, Timestamp ts, Event* out,
                                  std::size_t out_capacity) noexcept {
    if (!id.valid() || out == nullptr || out_capacity == 0) {
        return {Error::InvalidArgs, 0};
    }
    const auto fit = by_id_.find(id.id);
    if (fit == by_id_.end()) {
        return {Error::UnknownId, 0};
    }
    const Loc loc = fit->second;
    const Quantity cancelled = loc.it->qty;
    const Price p = loc.it->price;
    auto& q = book(loc.side)[loc.price_ticks];
    q.erase(loc.it);
    if (q.empty()) {
        book(loc.side).erase(loc.price_ticks);
    }
    by_id_.erase(fit);
    --live_;
    std::size_t n = 0;
    emit(EventType::Cancel, id, ts, p, cancelled, out, n);
    return {Error::Ok, n};
}

RefBook::OpResult RefBook::modify(OrderId id, Price new_price, Quantity new_qty,
                                  Timestamp ts, Event* out,
                                  std::size_t out_capacity) noexcept {
    if (!id.valid() || new_qty.lots <= 0 || out == nullptr || out_capacity == 0) {
        return {Error::InvalidArgs, 0};
    }
    const auto fit = by_id_.find(id.id);
    if (fit == by_id_.end()) {
        return {Error::UnknownId, 0};
    }
    if (out_capacity < live_ + 2) {
        return {Error::EventBufferFull, 0};
    }

    Loc loc = fit->second;
    RefOrder& o = *loc.it;
    if (new_price.ticks == o.price.ticks && new_qty.lots <= o.qty.lots) {
        o.qty.lots = new_qty.lots;
        std::size_t n = 0;
        emit(EventType::Modify, id, ts, o.price, new_qty, out, n);
        return {Error::Ok, n};
    }

    if (!in_band(new_price)) {
        return {Error::OutOfBandPrice, 0};
    }
    if (o.type == OrderType::PostOnly) {
        auto& opp = (o.side == Side::Bid) ? asks_ : bids_;
        bool crosses = false;
        if (!opp.empty()) {
            const std::int64_t touch = (o.side == Side::Bid)
                                           ? opp.begin()->first
                                           : opp.rbegin()->first;
            crosses = (o.side == Side::Bid) ? new_price.ticks >= touch
                                            : new_price.ticks <= touch;
        }
        if (crosses) {
            std::size_t n = 0;
            emit(EventType::Reject, id, ts, new_price, new_qty, out, n);
            return {Error::WouldCrossPostOnly, n};
        }
    }

    const Side side = o.side;
    const OrderType type = o.type;
    const std::uint32_t owner = o.owner;
    const Quantity old_qty = o.qty;
    const Price old_price = o.price;
    auto& q = book(side)[loc.price_ticks];
    q.erase(loc.it);
    if (q.empty()) {
        book(side).erase(loc.price_ticks);
    }
    by_id_.erase(fit);
    --live_;

    std::size_t n = 0;
    emit(EventType::Cancel, id, ts, old_price, old_qty, out, n);
    const Error err =
        execute_add(id, side, type, new_price,
                    static_cast<std::uint64_t>(new_qty.lots), ts, owner, out, n);
    return {err, n};
}

}  // namespace reference
}  // namespace kairos
