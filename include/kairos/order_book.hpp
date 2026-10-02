#pragma once

// Limit order book with price-time priority.
//
// Design (see docs/design/orderbook.md for alternatives and trade-offs):
//   - Price levels: fixed array indexed by tick offset from a configurable
//     band. O(1) level lookup, contiguous memory.
//   - FIFO queue per level: intrusive doubly-linked list via pool indices.
//     O(1) enqueue/dequeue/cancel-from-middle, zero allocation.
//   - Orders: pre-allocated pool with a free list; handles are
//     index+generation to defeat ABA bugs.
//   - OrderId -> order: flat open-addressing hash map (linear probing),
//     fixed capacity, no allocation after construction.
//   - Best bid/ask: hierarchical bitmap over levels; hardware bit-scan
//     instructions give effectively O(1) touch lookup.
//   - Output: caller-provided event buffer; the engine never allocates.
//   - Errors: returned as Error codes; the hot path is noexcept.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "kairos/error.hpp"
#include "kairos/events.hpp"
#include "kairos/types.hpp"

namespace kairos {

inline constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

// Order types. IOC/FOK/PostOnly are limit orders with execution constraints;
// Market ignores the price argument.
enum class OrderType : std::uint8_t { Limit, Market, Ioc, Fok, PostOnly };

enum class SelfTradePolicy : std::uint8_t {
    Allow,           // no self-trade handling (default)
    CancelResting,   // remove the resting order, incoming order continues
    CancelIncoming,  // stop and reject the incoming order's remainder
};
// Self-trade logic only applies between two *nonzero, equal* owners.
// owner == 0 means "anonymous": with the default policy the check is a no-op,
// so single-trader simulation needs no configuration. Multi-participant use
// must assign distinct nonzero owners per participant.

struct OrderBookConfig {
    Price base_price{0};                 // center of the price band
    std::uint32_t half_band = 1024;      // levels on each side of base_price
    std::uint32_t max_orders = 1u << 20; // order pool capacity
    SelfTradePolicy self_trade = SelfTradePolicy::Allow;
};

// Intrusive order node. All fields are plain data; links are pool indices so
// they never invalidate and serialize trivially. Target: <= 64 bytes.
struct Order {
    OrderId id{0};
    Price price{0};
    Quantity remaining{0};
    std::uint64_t seq = 0;               // insertion sequence (time priority)
    std::uint32_t prev = kInvalidIndex;  // level-queue link
    std::uint32_t next = kInvalidIndex;  // level-queue link
    std::uint32_t generation = 0;        // ABA guard on pool-slot reuse
    std::uint32_t owner = 0;             // 0 = anonymous
    Side side = Side::Bid;
    OrderType type = OrderType::Limit;
};
static_assert(sizeof(Order) <= 64);
static_assert(std::is_trivially_copyable_v<Order>);

struct LevelQueue {
    std::uint32_t head = kInvalidIndex;  // oldest order (price-time priority)
    std::uint32_t tail = kInvalidIndex;  // newest order
    std::uint32_t count = 0;
    Quantity total{0};
};

struct Level {
    LevelQueue bids;
    LevelQueue asks;
};

class OrderBook {
public:
    explicit OrderBook(OrderBookConfig cfg = OrderBookConfig{});

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    struct OpResult {
        Error error = Error::Ok;
        std::size_t events = 0;  // events written to out
    };

    // Add an order. Writes fills (taker perspective) followed by exactly one
    // disposition event (Ack, or Reject for post-only/FOK/self-trade rejects)
    // into out[0, out_capacity). If out is too small, NOTHING is mutated and
    // EventBufferFull is returned: operations are atomic wrt the event buffer.
    // At most live_count()+1 events are ever needed for add.
    OpResult add(OrderId id, Side side, OrderType type, Price price, Quantity qty,
                 Timestamp ts, std::uint32_t owner,
                 Event* out, std::size_t out_capacity) noexcept;

    // Cancel a resting order. Exactly one Cancel event on success.
    OpResult cancel(OrderId id, Timestamp ts,
                    Event* out, std::size_t out_capacity) noexcept;

    // Modify price and/or quantity.
    //   - Same price and non-increased quantity: in-place, keeps queue
    //     priority, emits one Modify event.
    //   - Otherwise: cancel/replace — the order loses priority. Emits one
    //     Cancel (old state) followed by the add sequence (fills + Ack)
    //     under the same id.
    OpResult modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts,
                    Event* out, std::size_t out_capacity) noexcept;

    bool best_bid(Price& out) const noexcept;
    bool best_ask(Price& out) const noexcept;
    std::size_t resting_count() const noexcept { return live_count_; }

    // Test/diagnostic only: O(n) full invariant scan (no crossed book, level
    // aggregates match, id map <-> pool consistent, free list integrity,
    // bitmap matches non-empty levels).
    bool debug_check_invariants() const noexcept;

private:
    // Level index for a price, or kInvalidIndex if outside the band.
    std::uint32_t level_index(Price p) const noexcept;

    std::uint32_t alloc_order() noexcept;
    void free_order(std::uint32_t idx) noexcept;

    std::uint32_t id_find(OrderId id) const noexcept;  // pool idx or kInvalidIndex
    void id_insert(OrderId id, std::uint32_t pool_idx) noexcept;
    void id_erase(OrderId id) noexcept;

    void queue_push(LevelQueue& q, std::uint32_t idx) noexcept;
    void queue_remove(LevelQueue& q, std::uint32_t idx) noexcept;

    void bitmap_set(bool is_bid, std::uint32_t lvl) noexcept;
    void bitmap_clear(bool is_bid, std::uint32_t lvl) noexcept;

    // Resting-order event emission helper (assigns seq).
    void emit(EventType type, OrderId id, Timestamp ts, Price price, Quantity qty,
              Event* out, std::size_t& n) noexcept;

    // Quantity fillable by a taker at or through `limit` (for FOK checks).
    // For market orders pass a limit that always crosses.
    std::uint64_t fillable_qty(Side taker_side, Price limit, std::uint64_t need,
                               std::uint32_t owner) const noexcept;

    // Core matching + rest. Preconditions: validation (args, duplicate,
    // capacity, band, post-only, FOK, event capacity) already done; pool slot
    // `idx` is reserved for this order (freshly allocated or reused); id not
    // in the map. Emits fills (taker perspective) then exactly one Ack
    // (unless rejected by self-trade policy). Returns Ok, or SelfTradeReject
    // when the self-trade policy stopped the order (fills already emitted
    // stand).
    Error execute_add(std::uint32_t idx, OrderId id, Side side, OrderType type,
                      Price price, std::uint64_t qty_lots, Timestamp ts,
                      std::uint32_t owner, std::uint32_t lvl,
                      Event* out, std::size_t& n) noexcept;

    OrderBookConfig cfg_;
    std::uint32_t n_levels_ = 0;
    std::int64_t band_lo_ = 0;  // inclusive tick of level 0

    std::vector<Level> levels_;
    std::vector<Order> pool_;
    std::vector<std::uint32_t> free_;  // stack of free pool indices

    struct IdEntry {
        OrderId key{0};  // key{0} == empty slot (OrderId 0 is invalid)
        std::uint32_t idx = kInvalidIndex;
    };
    std::vector<IdEntry> id_map_;
    std::uint32_t id_mask_ = 0;

    std::vector<std::uint64_t> bid_bits0_;
    std::vector<std::uint64_t> ask_bits0_;
    std::uint64_t bid_bits1_ = 0;
    std::uint64_t ask_bits1_ = 0;

    std::uint64_t next_event_seq_ = 1;  // dense event sequence numbers
    std::uint64_t next_order_seq_ = 1;  // time-priority sequence for resting orders
    std::size_t live_count_ = 0;
};

}  // namespace kairos
