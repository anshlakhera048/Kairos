// OrderBook implementation: price-time priority matching engine.
//
// Single-threaded, allocation-free after construction, noexcept throughout.
// See include/kairos/order_book.hpp and docs/design/orderbook.md.

#include "kairos/order_book.hpp"

#include <algorithm>
#include <bit>

namespace kairos {
namespace {

// Deterministic 64-bit mixer for the id hash map (splitmix64).
std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

}  // namespace

OrderBook::OrderBook(OrderBookConfig cfg) : cfg_(cfg) {
    n_levels_ = 2 * cfg_.half_band;
    band_lo_ = cfg_.base_price.ticks - static_cast<std::int64_t>(cfg_.half_band);
    levels_.resize(n_levels_);

    pool_.resize(cfg_.max_orders);
    free_.reserve(cfg_.max_orders);
    for (std::uint32_t i = cfg_.max_orders; i-- > 0;) {
        free_.push_back(i);
    }

    std::uint64_t id_cap = 1;
    while (id_cap < 2 * static_cast<std::uint64_t>(cfg_.max_orders)) {
        id_cap <<= 1;
    }
    id_map_.resize(static_cast<std::size_t>(id_cap));
    id_mask_ = static_cast<std::uint32_t>(id_cap - 1);

    const std::uint32_t words = (n_levels_ + 63) / 64;
    bid_bits0_.assign(words, 0);
    ask_bits0_.assign(words, 0);
}

std::uint32_t OrderBook::level_index(Price p) const noexcept {
    const std::int64_t off = p.ticks - band_lo_;
    if (off < 0 || off >= static_cast<std::int64_t>(n_levels_)) {
        return kInvalidIndex;
    }
    return static_cast<std::uint32_t>(off);
}

std::uint32_t OrderBook::alloc_order() noexcept {
    const std::uint32_t idx = free_.back();
    free_.pop_back();
    Order& o = pool_[idx];
    ++o.generation;
    o.prev = kInvalidIndex;
    o.next = kInvalidIndex;
    ++live_count_;
    return idx;
}

void OrderBook::free_order(std::uint32_t idx) noexcept {
    free_.push_back(idx);
    --live_count_;
}

std::uint32_t OrderBook::id_find(OrderId id) const noexcept {
    std::uint64_t h = splitmix64(id.id) & id_mask_;
    for (;;) {
        const IdEntry& e = id_map_[static_cast<std::size_t>(h)];
        if (e.key.id == 0) {
            return kInvalidIndex;
        }
        if (e.key.id == id.id) {
            return e.idx;
        }
        h = (h + 1) & id_mask_;
    }
}

void OrderBook::id_insert(OrderId id, std::uint32_t pool_idx) noexcept {
    // Caller guarantees: id not present, map not full (load <= 0.5 always,
    // since live orders <= max_orders < capacity / 2).
    std::uint64_t h = splitmix64(id.id) & id_mask_;
    for (;;) {
        IdEntry& e = id_map_[static_cast<std::size_t>(h)];
        if (e.key.id == 0) {
            e.key = id;
            e.idx = pool_idx;
            return;
        }
        h = (h + 1) & id_mask_;
    }
}

void OrderBook::id_erase(OrderId id) noexcept {
    std::uint64_t h = splitmix64(id.id) & id_mask_;
    while (id_map_[static_cast<std::size_t>(h)].key.id != 0 &&
           id_map_[static_cast<std::size_t>(h)].key.id != id.id) {
        h = (h + 1) & id_mask_;
    }
    if (id_map_[static_cast<std::size_t>(h)].key.id == 0) {
        return;  // absent; nothing to do
    }
    // Backward-shift deletion: fill the hole by shifting the cluster back,
    // preserving the linear-probing invariant. Table is never full, so the
    // inner scan always terminates at an empty slot.
    std::uint64_t hole = h;
    for (;;) {
        std::uint64_t j = (hole + 1) & id_mask_;
        for (;;) {
            if (id_map_[static_cast<std::size_t>(j)].key.id == 0) {
                id_map_[static_cast<std::size_t>(hole)].key = OrderId{0};
                id_map_[static_cast<std::size_t>(hole)].idx = kInvalidIndex;
                return;
            }
            const std::uint64_t k =
                splitmix64(id_map_[static_cast<std::size_t>(j)].key.id) & id_mask_;
            // Entry at j may move into hole iff hole lies on cyclic [k, j].
            const bool in_path =
                (k <= j) ? (k <= hole && hole <= j) : (hole >= k || hole <= j);
            if (in_path) {
                break;
            }
            j = (j + 1) & id_mask_;
        }
        id_map_[static_cast<std::size_t>(hole)] = id_map_[static_cast<std::size_t>(j)];
        hole = j;
    }
}

void OrderBook::queue_push(LevelQueue& q, std::uint32_t idx) noexcept {
    Order& o = pool_[idx];
    o.prev = q.tail;
    o.next = kInvalidIndex;
    if (q.tail != kInvalidIndex) {
        pool_[q.tail].next = idx;
    } else {
        q.head = idx;
    }
    q.tail = idx;
    ++q.count;
    q.total.lots += o.remaining.lots;
}

void OrderBook::queue_remove(LevelQueue& q, std::uint32_t idx) noexcept {
    Order& o = pool_[idx];
    if (o.prev != kInvalidIndex) {
        pool_[o.prev].next = o.next;
    } else {
        q.head = o.next;
    }
    if (o.next != kInvalidIndex) {
        pool_[o.next].prev = o.prev;
    } else {
        q.tail = o.prev;
    }
    o.prev = kInvalidIndex;
    o.next = kInvalidIndex;
    --q.count;
    q.total.lots -= o.remaining.lots;
}

void OrderBook::bitmap_set(bool is_bid, std::uint32_t lvl) noexcept {
    auto& l0 = is_bid ? bid_bits0_ : ask_bits0_;
    std::uint64_t& l1 = is_bid ? bid_bits1_ : ask_bits1_;
    l0[lvl >> 6] |= 1ULL << (lvl & 63);
    l1 |= 1ULL << (lvl >> 6);
}

void OrderBook::bitmap_clear(bool is_bid, std::uint32_t lvl) noexcept {
    auto& l0 = is_bid ? bid_bits0_ : ask_bits0_;
    std::uint64_t& l1 = is_bid ? bid_bits1_ : ask_bits1_;
    std::uint64_t& w = l0[lvl >> 6];
    w &= ~(1ULL << (lvl & 63));
    if (w == 0) {
        l1 &= ~(1ULL << (lvl >> 6));
    }
}

void OrderBook::emit(EventType type, OrderId id, Timestamp ts, Price price,
                     Quantity qty, Event* out, std::size_t& n) noexcept {
    out[n].type = type;
    out[n].order_id = id;
    out[n].ts = ts;
    out[n].seq = next_event_seq_++;
    out[n].price = price;
    out[n].qty = qty;
    ++n;
}

bool OrderBook::best_bid(Price& out) const noexcept {
    if (bid_bits1_ == 0) {
        return false;
    }
    const unsigned w = 63 - std::countl_zero(bid_bits1_);
    const std::uint64_t word = bid_bits0_[w];
    const unsigned b = 63 - std::countl_zero(word);
    out = Price{band_lo_ + static_cast<std::int64_t>((w << 6) + b)};
    return true;
}

bool OrderBook::best_ask(Price& out) const noexcept {
    if (ask_bits1_ == 0) {
        return false;
    }
    const unsigned w = static_cast<unsigned>(std::countr_zero(ask_bits1_));
    const std::uint64_t word = ask_bits0_[w];
    const unsigned b = static_cast<unsigned>(std::countr_zero(word));
    out = Price{band_lo_ + static_cast<std::int64_t>((w << 6) + b)};
    return true;
}

bool OrderBook::get_order(OrderId id, Order& out) const noexcept {
    const std::uint32_t idx = id_find(id);
    if (idx == kInvalidIndex) {
        return false;
    }
    out = pool_[idx];
    return true;
}

std::size_t OrderBook::snapshot_levels(LevelInfo* out, std::size_t max_levels,
                                       std::size_t per_side) const noexcept {
    std::size_t n = 0;
    // Bids: descending price (highest first).
    if (bid_bits1_ != 0) {
        std::uint64_t bits1 = bid_bits1_;
        std::size_t count = 0;
        while (bits1 != 0 && count < per_side && n < max_levels) {
            const unsigned w = 63 - std::countl_zero(bits1);
            std::uint64_t word = bid_bits0_[w];
            while (word != 0 && count < per_side && n < max_levels) {
                const unsigned b = 63 - std::countl_zero(word);
                const std::uint32_t lvl_idx = (w << 6) + b;
                const Level& lvl = levels_[lvl_idx];
                if (lvl.bids.total.lots > 0) {
                    out[n].price_ticks =
                        band_lo_ + static_cast<std::int64_t>(lvl_idx);
                    out[n].qty_lots = lvl.bids.total.lots;
                    out[n].is_bid = true;
                    ++n;
                    ++count;
                }
                word &= word - 1;  // clear lowest set bit
            }
            bits1 &= bits1 - 1;
        }
    }
    // Asks: ascending price (lowest first).
    if (ask_bits1_ != 0) {
        std::uint64_t bits1 = ask_bits1_;
        std::size_t count = 0;
        while (bits1 != 0 && count < per_side && n < max_levels) {
            const unsigned w = static_cast<unsigned>(std::countr_zero(bits1));
            std::uint64_t word = ask_bits0_[w];
            while (word != 0 && count < per_side && n < max_levels) {
                const unsigned b = static_cast<unsigned>(std::countr_zero(word));
                const std::uint32_t lvl_idx = (w << 6) + b;
                const Level& lvl = levels_[lvl_idx];
                if (lvl.asks.total.lots > 0) {
                    out[n].price_ticks =
                        band_lo_ + static_cast<std::int64_t>(lvl_idx);
                    out[n].qty_lots = lvl.asks.total.lots;
                    out[n].is_bid = false;
                    ++n;
                    ++count;
                }
                word &= word - 1;
            }
            bits1 &= bits1 - 1;
        }
    }
    return n;
}

// Read-only pre-scan: quantity a taker could fill at or through `limit`.
// Used for the FOK all-or-nothing check. Respects the self-trade policy:
// under CancelResting own orders contribute 0 (they would be cancelled);
// under CancelIncoming the scan stops at the first own order.
std::uint64_t OrderBook::fillable_qty(Side taker_side, Price limit, std::uint64_t need,
                                      std::uint32_t owner) const noexcept {
    std::uint64_t fillable = 0;
    const bool taker_is_bid = (taker_side == Side::Bid);
    const std::vector<std::uint64_t>& l0 = taker_is_bid ? ask_bits0_ : bid_bits0_;
    std::uint64_t l1 = taker_is_bid ? ask_bits1_ : bid_bits1_;

    if (taker_is_bid) {
        // Asks, ascending from the touch.
        while (l1 != 0 && fillable < need) {
            const unsigned w = static_cast<unsigned>(std::countr_zero(l1));
            std::uint64_t word = l0[w];
            while (word != 0 && fillable < need) {
                const unsigned b = static_cast<unsigned>(std::countr_zero(word));
                const std::uint32_t li = (w << 6) + b;
                const Price p{band_lo_ + static_cast<std::int64_t>(li)};
                if (p.ticks > limit.ticks) {
                    return fillable < need ? fillable : need;
                }
                std::uint32_t oi = levels_[li].asks.head;
                while (oi != kInvalidIndex && fillable < need) {
                    const Order& r = pool_[oi];
                    const bool self = (owner != 0 && r.owner == owner &&
                                       cfg_.self_trade != SelfTradePolicy::Allow);
                    if (self && cfg_.self_trade == SelfTradePolicy::CancelIncoming) {
                        return fillable < need ? fillable : need;
                    }
                    if (!self) {
                        fillable += static_cast<std::uint64_t>(r.remaining.lots);
                    }
                    oi = r.next;
                }
                word &= word - 1;
            }
            l1 &= l1 - 1;
        }
    } else {
        // Bids, descending from the touch.
        while (l1 != 0 && fillable < need) {
            const unsigned w = 63 - std::countl_zero(l1);
            std::uint64_t word = l0[w];
            while (word != 0 && fillable < need) {
                const unsigned b = 63 - std::countl_zero(word);
                const std::uint32_t li = (w << 6) + b;
                const Price p{band_lo_ + static_cast<std::int64_t>(li)};
                if (p.ticks < limit.ticks) {
                    return fillable < need ? fillable : need;
                }
                std::uint32_t oi = levels_[li].bids.head;
                while (oi != kInvalidIndex && fillable < need) {
                    const Order& r = pool_[oi];
                    const bool self = (owner != 0 && r.owner == owner &&
                                       cfg_.self_trade != SelfTradePolicy::Allow);
                    if (self && cfg_.self_trade == SelfTradePolicy::CancelIncoming) {
                        return fillable < need ? fillable : need;
                    }
                    if (!self) {
                        fillable += static_cast<std::uint64_t>(r.remaining.lots);
                    }
                    oi = r.next;
                }
                word &= ~(1ULL << b);
            }
            l1 &= ~(1ULL << w);
        }
    }
    return fillable < need ? fillable : need;
}

// Core matching + rest. Preconditions: validation (args, duplicate, capacity,
// band, post-only, FOK, event capacity) already done; pool slot `idx` is
// reserved for this order (freshly allocated or reused); id not in the map.
// Emits fills (taker perspective) then exactly one Ack (unless rejected by
// self-trade policy).
Error OrderBook::execute_add(std::uint32_t idx, OrderId id, Side side, OrderType type,
                            Price price, std::uint64_t qty_lots, Timestamp ts,
                            std::uint32_t owner, std::uint32_t lvl,
                            Event* out, std::size_t& n) noexcept {
    std::uint64_t remaining = qty_lots;
    const bool taker_is_bid = (side == Side::Bid);
    const bool is_market = (type == OrderType::Market);
    bool rejected = false;

    while (remaining > 0 && !rejected) {
        Price best{0};
        const bool has = taker_is_bid ? best_ask(best) : best_bid(best);
        if (!has) {
            break;
        }
        if (!is_market) {
            if (taker_is_bid && best.ticks > price.ticks) {
                break;
            }
            if (!taker_is_bid && best.ticks < price.ticks) {
                break;
            }
        }
        const std::uint32_t li = level_index(best);
        Level& level = levels_[li];
        LevelQueue& q = taker_is_bid ? level.asks : level.bids;

        while (remaining > 0 && q.head != kInvalidIndex) {
            const std::uint32_t ridx = q.head;
            const Order& ro = pool_[ridx];
            const bool self = (owner != 0 && ro.owner == owner &&
                               cfg_.self_trade != SelfTradePolicy::Allow);

            if (self && cfg_.self_trade == SelfTradePolicy::CancelIncoming) {
                emit(EventType::Reject, id, ts, price,
                     Quantity{static_cast<std::int64_t>(remaining)}, out, n);
                rejected = true;
                remaining = 0;
                break;
            }
            if (self) {  // CancelResting: remove the resting order, continue.
                const OrderId rid = ro.id;
                const Price rp = ro.price;
                const Quantity rq = ro.remaining;
                queue_remove(q, ridx);
                id_erase(rid);
                free_order(ridx);
                if (q.count == 0) {
                    bitmap_clear(!taker_is_bid, li);
                }
                emit(EventType::Cancel, rid, ts, rp, rq, out, n);
                continue;
            }

            const std::uint64_t fill =
                std::min(remaining, static_cast<std::uint64_t>(ro.remaining.lots));
            Order& r = pool_[ridx];
            r.remaining.lots -= static_cast<std::int64_t>(fill);
            q.total.lots -= static_cast<std::int64_t>(fill);
            remaining -= fill;
            emit(EventType::Fill, id, ts, r.price,
                 Quantity{static_cast<std::int64_t>(fill)}, out, n);
            if (r.remaining.lots == 0) {
                const OrderId rid = r.id;
                queue_remove(q, ridx);
                id_erase(rid);
                free_order(ridx);
                if (q.count == 0) {
                    bitmap_clear(!taker_is_bid, li);
                }
            }
        }
    }

    // Only Limit and PostOnly can rest. (PostOnly was validated non-crossing
    // above, so a non-crossing remainder always rests.)
    const bool can_rest =
        (type == OrderType::Limit || type == OrderType::PostOnly);
    const bool rests = (!rejected && can_rest && remaining > 0);
    if (rests) {
        Order& o = pool_[idx];
        o.id = id;
        o.price = price;
        o.remaining = Quantity{static_cast<std::int64_t>(remaining)};
        o.seq = next_order_seq_++;
        o.owner = owner;
        o.side = side;
        o.type = type;
        Level& l = levels_[lvl];
        LevelQueue& q = (side == Side::Bid) ? l.bids : l.asks;
        const bool was_empty = (q.count == 0);
        queue_push(q, idx);
        id_insert(id, idx);
        if (was_empty) {
            bitmap_set(side == Side::Bid, lvl);
        }
    } else {
        free_order(idx);  // fully filled, IOC/Market remainder, or rejected
    }

    if (!rejected) {
        // Ack carries the RESTING quantity: 0 when the remainder was filled
        // or discarded (IOC/Market), nonzero only when it rests in the book.
        const std::int64_t resting_qty = rests ? static_cast<std::int64_t>(remaining) : 0;
        emit(EventType::Ack, id, ts, price, Quantity{resting_qty}, out, n);
    }
    return rejected ? Error::SelfTradeReject : Error::Ok;
}

OrderBook::OpResult OrderBook::add(OrderId id, Side side, OrderType type, Price price,
                                   Quantity qty, Timestamp ts, std::uint32_t owner,
                                   Event* out, std::size_t out_capacity) noexcept {
    if (!id.valid() || qty.lots <= 0 || out == nullptr || out_capacity == 0) {
        return {Error::InvalidArgs, 0};
    }
    if (id_find(id) != kInvalidIndex) {
        return {Error::DuplicateId, 0};
    }
    if (free_.empty()) {
        return {Error::NoCapacity, 0};
    }

    const bool is_market = (type == OrderType::Market);
    std::uint32_t lvl = kInvalidIndex;
    if (!is_market) {
        lvl = level_index(price);
        if (lvl == kInvalidIndex) {
            return {Error::OutOfBandPrice, 0};
        }
    }

    // Atomicity wrt the event buffer: an add emits at most live_count_+1
    // events (one per resting order touched, plus the Ack). If the buffer is
    // too small, reject before touching any state.
    if (out_capacity < live_count_ + 1) {
        return {Error::EventBufferFull, 0};
    }

    if (type == OrderType::PostOnly) {
        Price touch{0};
        const bool has = (side == Side::Bid) ? best_ask(touch) : best_bid(touch);
        const bool crosses =
            has && (side == Side::Bid ? price.ticks >= touch.ticks
                                      : price.ticks <= touch.ticks);
        if (crosses) {
            std::size_t n = 0;
            emit(EventType::Reject, id, ts, price, qty, out, n);
            return {Error::WouldCrossPostOnly, n};
        }
    }

    if (type == OrderType::Fok &&
        fillable_qty(side, price, static_cast<std::uint64_t>(qty.lots), owner) <
            static_cast<std::uint64_t>(qty.lots)) {
        std::size_t n = 0;
        emit(EventType::Reject, id, ts, price, qty, out, n);
        return {Error::InsufficientQtyFok, n};
    }

    const std::uint32_t idx = alloc_order();
    std::size_t n = 0;
    const Error err =
        execute_add(idx, id, side, type, price, static_cast<std::uint64_t>(qty.lots),
                    ts, owner, lvl, out, n);
    return {err, n};
}

OrderBook::OpResult OrderBook::cancel(OrderId id, Timestamp ts, Event* out,
                                      std::size_t out_capacity) noexcept {
    if (!id.valid() || out == nullptr || out_capacity == 0) {
        return {Error::InvalidArgs, 0};
    }
    const std::uint32_t idx = id_find(id);
    if (idx == kInvalidIndex) {
        return {Error::UnknownId, 0};
    }
    const Order& o = pool_[idx];
    const Quantity cancelled = o.remaining;
    const Price p = o.price;
    const bool is_bid = (o.side == Side::Bid);
    const std::uint32_t li = level_index(p);
    Level& l = levels_[li];
    LevelQueue& q = is_bid ? l.bids : l.asks;
    queue_remove(q, idx);
    id_erase(id);
    free_order(idx);
    if (q.count == 0) {
        bitmap_clear(is_bid, li);
    }
    std::size_t n = 0;
    emit(EventType::Cancel, id, ts, p, cancelled, out, n);
    return {Error::Ok, n};
}

OrderBook::OpResult OrderBook::modify(OrderId id, Price new_price, Quantity new_qty,
                                      Timestamp ts, Event* out,
                                      std::size_t out_capacity) noexcept {
    if (!id.valid() || new_qty.lots <= 0 || out == nullptr || out_capacity == 0) {
        return {Error::InvalidArgs, 0};
    }
    const std::uint32_t idx = id_find(id);
    if (idx == kInvalidIndex) {
        return {Error::UnknownId, 0};
    }
    // Replace path emits at most live_count_+2 events (Cancel + fills + Ack).
    if (out_capacity < live_count_ + 2) {
        return {Error::EventBufferFull, 0};
    }

    Order& o = pool_[idx];
    if (new_price.ticks == o.price.ticks && new_qty.lots <= o.remaining.lots) {
        // In-place quantity reduction: keeps queue priority.
        const std::int64_t delta = o.remaining.lots - new_qty.lots;
        o.remaining.lots = new_qty.lots;
        const std::uint32_t li = level_index(o.price);
        LevelQueue& q = (o.side == Side::Bid) ? levels_[li].bids : levels_[li].asks;
        q.total.lots -= delta;
        std::size_t n = 0;
        emit(EventType::Modify, id, ts, o.price, new_qty, out, n);
        return {Error::Ok, n};
    }

    // Cancel/replace: price change or quantity increase loses time priority.
    const std::uint32_t new_lvl = level_index(new_price);
    if (new_lvl == kInvalidIndex) {
        return {Error::OutOfBandPrice, 0};
    }
    if (o.type == OrderType::PostOnly) {
        Price touch{0};
        const bool has =
            (o.side == Side::Bid) ? best_ask(touch) : best_bid(touch);
        // Note: the order itself is still resting, so the touch may be its
        // own price; crossing means strictly through it.
        const bool crosses =
            has && (o.side == Side::Bid ? new_price.ticks >= touch.ticks
                                        : new_price.ticks <= touch.ticks);
        if (crosses) {
            std::size_t n = 0;
            emit(EventType::Reject, id, ts, new_price, new_qty, out, n);
            return {Error::WouldCrossPostOnly, n};
        }
    }

    // Capture, then remove the old order. The pool slot is reused, not freed.
    const Side side = o.side;
    const OrderType type = o.type;
    const std::uint32_t owner = o.owner;
    const Quantity old_qty = o.remaining;
    const Price old_price = o.price;
    {
        const std::uint32_t li = level_index(old_price);
        LevelQueue& q = (side == Side::Bid) ? levels_[li].bids : levels_[li].asks;
        queue_remove(q, idx);
        id_erase(id);
        if (q.count == 0) {
            bitmap_clear(side == Side::Bid, li);
        }
    }

    std::size_t n = 0;
    emit(EventType::Cancel, id, ts, old_price, old_qty, out, n);

    // Re-initialize the slot as a fresh order under the same id.
    Order& r = pool_[idx];
    ++r.generation;
    r.price = new_price;
    r.remaining = new_qty;
    r.prev = kInvalidIndex;
    r.next = kInvalidIndex;
    // id, side, type, owner unchanged.

    const Error err =
        execute_add(idx, id, side, type, new_price,
                    static_cast<std::uint64_t>(new_qty.lots), ts, owner, new_lvl,
                    out, n);
    return {err, n};
}

bool OrderBook::debug_check_invariants() const noexcept {
    std::vector<char> seen(pool_.size(), 0);
    std::size_t counted_live = 0;

    const auto& bits0 = [](bool is_bid, const OrderBook* self) -> const std::vector<std::uint64_t>& {
        return is_bid ? self->bid_bits0_ : self->ask_bits0_;
    };

    for (std::uint32_t li = 0; li < n_levels_; ++li) {
        for (int s = 0; s < 2; ++s) {
            const bool is_bid = (s == 0);
            const LevelQueue& q = is_bid ? levels_[li].bids : levels_[li].asks;
            const bool bit =
                ((bits0(is_bid, this)[li >> 6] >> (li & 63)) & 1ULL) != 0;
            if (bit != (q.count > 0)) {
                return false;
            }
            std::int64_t sum = 0;
            std::uint32_t cnt = 0;
            std::uint32_t cur = q.head;
            std::uint32_t prev = kInvalidIndex;
            while (cur != kInvalidIndex) {
                if (cur >= pool_.size() || seen[cur]) {
                    return false;
                }
                seen[static_cast<std::size_t>(cur)] = 1;
                const Order& o = pool_[cur];
                if (o.prev != prev) {
                    return false;
                }
                if (o.side != (is_bid ? Side::Bid : Side::Ask)) {
                    return false;
                }
                if (level_index(o.price) != li) {
                    return false;
                }
                if (o.remaining.lots <= 0) {
                    return false;
                }
                sum += o.remaining.lots;
                ++cnt;
                if (cnt > q.count) {
                    return false;  // cycle guard
                }
                prev = cur;
                cur = o.next;
            }
            if (cnt != q.count || sum != q.total.lots) {
                return false;
            }
            if ((q.head == kInvalidIndex) != (q.tail == kInvalidIndex)) {
                return false;
            }
            if (q.count == 0 && q.total.lots != 0) {
                return false;
            }
            counted_live += cnt;
        }
    }

    // No crossed book.
    Price bb{0}, ba{0};
    if (best_bid(bb) && best_ask(ba) && bb.ticks >= ba.ticks) {
        return false;
    }

    // Id map <-> pool consistency.
    std::size_t map_count = 0;
    for (const IdEntry& e : id_map_) {
        if (e.key.id == 0) {
            continue;
        }
        ++map_count;
        if (e.idx >= pool_.size() || pool_[e.idx].id.id != e.key.id) {
            return false;
        }
        if (!seen[e.idx]) {
            return false;
        }
    }
    if (map_count != counted_live) {
        return false;
    }

    // Free list integrity: every slot is either live or free, exactly once.
    if (free_.size() + counted_live != pool_.size()) {
        return false;
    }
    for (std::uint32_t fi : free_) {
        if (fi >= pool_.size() || seen[fi]) {
            return false;
        }
    }
    return true;
}

}  // namespace kairos
