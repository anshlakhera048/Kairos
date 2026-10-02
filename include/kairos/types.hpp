#pragma once

// Core value types for Kairos.
//
// Every price and quantity in the engine is a fixed-point integer, never a
// floating-point number. Rationale:
//   - Determinism: integer arithmetic is exact and identical across compilers
//     and CPUs. FP rounding can differ by platform and breaks bit-identical
//     replay (a non-negotiable rule of this project).
//   - Performance: integer compare/add is single-cycle; no FP pipeline stalls
//     on the hot path.
//   - Correctness: prices compare exactly. No epsilon hacks, no "almost equal".

#include <compare>
#include <cstdint>
#include <type_traits>

namespace kairos {

// Price stored as integer ticks: real_price = ticks * tick_size, where
// tick_size is a property of the instrument (kept outside the hot path).
//
// int64, not uint64: spreads and price differences are signed quantities, and
// we want a negative sentinel (e.g. Price{-1} == "no price") without wrapping.
struct Price {
    std::int64_t ticks = 0;

    constexpr explicit Price(std::int64_t ticks_in) noexcept : ticks(ticks_in) {}

    constexpr auto operator<=>(const Price&) const noexcept = default;
};

// Quantity stored as integer lots: real_qty = lots * lot_size.
// int64, not uint64: fills and position deltas are signed.
struct Quantity {
    std::int64_t lots = 0;

    constexpr explicit Quantity(std::int64_t lots_in) noexcept : lots(lots_in) {}

    constexpr auto operator<=>(const Quantity&) const noexcept = default;
};

// Order identifier. uint64 gives a vast id space; 0 is reserved as the
// "invalid / unassigned" sentinel, so valid ids start at 1.
struct OrderId {
    std::uint64_t id = 0;

    constexpr explicit OrderId(std::uint64_t id_in) noexcept : id(id_in) {}

    constexpr auto operator<=>(const OrderId&) const noexcept = default;
    constexpr bool valid() const noexcept { return id != 0; }
};

// Timestamp in nanoseconds. Time is an EXPLICIT input to the core (never read
// from a wall clock inside the engine) so that replays are deterministic.
// Both monotonic and wall-clock stamps are recorded at the edge; the core
// only ever sees this one integer.
struct Timestamp {
    std::uint64_t ns = 0;

    constexpr explicit Timestamp(std::uint64_t ns_in) noexcept : ns(ns_in) {}

    constexpr auto operator<=>(const Timestamp&) const noexcept = default;
};

// Why thin wrapper structs instead of raw ints or `using` aliases?
//   1. Type safety: Price{100} and Quantity{100} are different types, so the
//      compiler rejects add(Price, Quantity) argument mix-ups at zero cost.
//      A `using Price = int64_t;` alias gives no such protection.
//   2. Zero cost: each wrapper has exactly the layout of its primitive
//      (verified by the static_asserts below), so it compiles away entirely.
//      No extra instructions, no padding, standard-layout + trivially copyable.
//   3. Explicit construction (`explicit`) prevents accidental implicit
//      conversions from raw integers at call sites.
enum class Side : std::uint8_t { Bid = 0, Ask = 1 };

// Bid = 0, Ask = 1 is deliberate: a Side converts directly to an index into
// side-indexed arrays, e.g. levels[static_cast<std::size_t>(side)].
constexpr Side opposite(Side s) noexcept {
    return s == Side::Bid ? Side::Ask : Side::Bid;
}

// The wrappers must add no size, no alignment and no overhead vs. the raw
// primitives. If any of these fire, the "zero-cost" claim is broken.
static_assert(sizeof(Price) == sizeof(std::int64_t));
static_assert(alignof(Price) == alignof(std::int64_t));
static_assert(std::is_trivially_copyable_v<Price>);
static_assert(std::is_standard_layout_v<Price>);

static_assert(sizeof(Quantity) == sizeof(std::int64_t));
static_assert(alignof(Quantity) == alignof(std::int64_t));
static_assert(std::is_trivially_copyable_v<Quantity>);
static_assert(std::is_standard_layout_v<Quantity>);

static_assert(sizeof(OrderId) == sizeof(std::uint64_t));
static_assert(alignof(OrderId) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<OrderId>);
static_assert(std::is_standard_layout_v<OrderId>);

static_assert(sizeof(Timestamp) == sizeof(std::uint64_t));
static_assert(alignof(Timestamp) == alignof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<Timestamp>);
static_assert(std::is_standard_layout_v<Timestamp>);

static_assert(sizeof(Side) == sizeof(std::uint8_t));

}  // namespace kairos
