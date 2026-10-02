// Stub implementations. Phase 1 replaces these with the real price-time
// priority matching engine. They exist now so the build, tests and
// benchmarks link and run end to end.

#include "kairos/order_book.hpp"

namespace kairos {

bool OrderBook::add(OrderId /*id*/, Side /*side*/, Price /*price*/,
                    Quantity /*qty*/) noexcept {
    return false;  // Phase 1: insert into book / match.
}

bool OrderBook::cancel(OrderId /*id*/) noexcept {
    return false;  // Phase 1: remove from book.
}

}  // namespace kairos
