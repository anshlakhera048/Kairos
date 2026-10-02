#pragma once

// Error handling without exceptions.
//
// Every fallible engine operation returns an Error by value. There are no
// exceptions anywhere in the core (the hot path must be noexcept), no
// errno-style globals, and no boolean traps that hide *which* error occurred.
// Value-producing calls return a small Result struct instead.

#include <cstdint>

namespace kairos {

enum class Error : std::uint8_t {
    Ok = 0,

    // Validation failures: no state was changed.
    InvalidArgs,           // null id, non-positive quantity, null event buffer
    DuplicateId,           // order id already resting
    UnknownId,             // cancel/modify of an id that is not resting
    OutOfBandPrice,        // limit price outside the configured tick band
    NoCapacity,            // order pool exhausted
    EventBufferFull,       // caller-provided event buffer too small (no state changed)

    // Order-type rejections: a Reject event is emitted, no state changed.
    WouldCrossPostOnly,    // post-only order would have crossed the touch
    InsufficientQtyFok,    // fill-or-kill could not be fully filled
    SelfTradeReject,       // incoming order cancelled by self-trade policy
};

constexpr const char* to_string(Error e) noexcept {
    switch (e) {
        case Error::Ok: return "Ok";
        case Error::InvalidArgs: return "InvalidArgs";
        case Error::DuplicateId: return "DuplicateId";
        case Error::UnknownId: return "UnknownId";
        case Error::OutOfBandPrice: return "OutOfBandPrice";
        case Error::NoCapacity: return "NoCapacity";
        case Error::EventBufferFull: return "EventBufferFull";
        case Error::WouldCrossPostOnly: return "WouldCrossPostOnly";
        case Error::InsufficientQtyFok: return "InsufficientQtyFok";
        case Error::SelfTradeReject: return "SelfTradeReject";
    }
    return "Unknown";
}

}  // namespace kairos
