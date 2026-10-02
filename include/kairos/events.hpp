#pragma once

// Events emitted by the engine: plain data, trivially copyable, fixed size.
//
// Rationale: events flow into a caller-provided buffer (stack array, ring
// buffer, mmap'd log) with no allocation. Trivially-copyable structs can be
// memcpy'd and replayed bit-identically. No std::string, no variants with
// heap state, no virtuals.
//
// Event stream contract (see docs/design/orderbook.md for the full spec):
//   - Every mutating operation emits exactly one *disposition* event
//     (Ack, Cancel, Modify, or Reject) plus zero or more Fill events.
//   - Fills are taker-perspective: one Fill per resting order consumed,
//     carrying the taker's order id.
//   - seq is a deterministic engine-assigned sequence number (starts at 1).
//     ts is the explicit timestamp passed in with the operation.

#include <cstdint>
#include <type_traits>

#include "kairos/types.hpp"

namespace kairos {

enum class EventType : std::uint8_t {
    Ack = 0,   // order accepted; qty = amount now resting (0 if fully filled/discarded)
    Reject,    // order refused; qty = unfilled remainder
    Fill,      // taker filled against one resting order; price/qty describe the fill
    Cancel,    // resting order cancelled; qty = cancelled quantity
    Modify,    // in-place quantity reduction; price/qty describe the new state
};

struct Event {
    EventType type = EventType::Ack;
    OrderId order_id{0};   // taker id for Fill/Ack/Reject; resting id for Cancel/Modify
    Timestamp ts{0};       // explicit operation timestamp (nanoseconds)
    std::uint64_t seq = 0; // deterministic engine sequence number
    Price price{0};         // fill price (Fill), limit price (Ack/Modify), 0 otherwise
    Quantity qty{0};        // filled qty (Fill), resting qty (Ack), etc. — see contract
};

static_assert(std::is_trivially_copyable_v<Event>);
static_assert(std::is_standard_layout_v<Event>);

}  // namespace kairos
