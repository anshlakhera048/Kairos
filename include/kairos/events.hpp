#pragma once

// Events emitted by the engine: plain data, trivially copyable, fixed size.
//
// Rationale: events flow into a ring buffer / append-only log with no
// allocation. Trivially-copyable structs can be memcpy'd, written to an
// mmap'd file, and replayed bit-identically. No std::string, no variants
// with heap state, no virtuals.

#include "kairos/types.hpp"

namespace kairos {

enum class EventType : std::uint8_t {
    Ack = 0,     // order accepted into the book
    Reject,      // order refused (post-only cross, invalid, duplicate id, ...)
    Fill,       // (possibly partial) execution against a resting order
    Cancel,      // resting order cancelled
    Modify      // order modified (cancel/replace semantics, see Phase 1)
};

struct Event {
    EventType type = EventType::Ack;
    OrderId order_id{0};
    Timestamp ts{0};     // engine sequence time, nanoseconds (explicit input)
    Price price{0};      // fill price for Fill; limit price otherwise
    Quantity qty{0};     // filled qty for Fill; remaining qty otherwise
};

static_assert(std::is_trivially_copyable_v<Event>);
static_assert(std::is_standard_layout_v<Event>);

}  // namespace kairos
