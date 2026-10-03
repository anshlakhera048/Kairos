// L2 book: sorted-vector implementation.

#include "kairos/l2_book.hpp"

#include <algorithm>

namespace kairos {

void L2Book::clear() {
    bids_.clear();
    asks_.clear();
}

void L2Book::upsert(bool is_bid, std::int64_t price, std::int64_t qty) {
    std::vector<L2Level>& v = is_bid ? bids_ : asks_;
    // Binary search. Bids descending, asks ascending.
    std::size_t lo = 0, hi = v.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const std::int64_t mp = v[mid].price_ticks;
        if (mp == price) {
            if (qty == 0) {
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(mid));
            } else {
                v[mid].qty_lots = qty;
            }
            return;
        }
        // Bids descending: if mp < price, target sorts before mid (lower index).
        // Asks ascending: if mp > price, target sorts before mid (lower index).
        const bool before_mid = is_bid ? (mp < price) : (mp > price);
        if (before_mid) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    if (qty != 0) {
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(lo), L2Level{price, qty});
    }
}

void L2Book::apply_snapshot(const data::EventView& ev) {
    clear();
    for (std::uint32_t i = 0; i < ev.header->n_levels; ++i) {
        const data::LevelEntry& e = ev.levels[i];
        if (e.qty_lots > 0) {
            upsert(e.side == 0, e.price_ticks, e.qty_lots);
        }
    }
}

void L2Book::apply_diff(const data::EventView& ev) {
    for (std::uint32_t i = 0; i < ev.header->n_levels; ++i) {
        const data::LevelEntry& e = ev.levels[i];
        upsert(e.side == 0, e.price_ticks, e.qty_lots);
    }
}

void L2Book::apply(const data::EventView& ev) {
    const auto t = static_cast<data::EventType>(ev.header->type);
    if (t == data::EventType::Snapshot) {
        apply_snapshot(ev);
    } else if (t == data::EventType::Diff) {
        apply_diff(ev);
    }
    // Trades do not change the L2 book directly (the exchange's diffs will
    // reflect any resulting level changes).
}

bool L2Book::best_bid(L2Level& out) const {
    if (bids_.empty()) {
        return false;
    }
    out = bids_.front();
    return true;
}

bool L2Book::best_ask(L2Level& out) const {
    if (asks_.empty()) {
        return false;
    }
    out = asks_.front();
    return true;
}

std::int64_t L2Book::level_qty(bool is_bid, std::int64_t price_ticks) const {
    const std::vector<L2Level>& v = is_bid ? bids_ : asks_;
    std::size_t lo = 0, hi = v.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const std::int64_t mp = v[mid].price_ticks;
        if (mp == price_ticks) {
            return v[mid].qty_lots;
        }
        const bool before_mid = is_bid ? (mp < price_ticks) : (mp > price_ticks);
        if (before_mid) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return 0;
}

bool L2Book::check_invariants() const {
    // Sorted order and no crossed book.
    for (std::size_t i = 1; i < bids_.size(); ++i) {
        if (bids_[i - 1].price_ticks <= bids_[i].price_ticks) {
            return false;
        }
        if (bids_[i].qty_lots <= 0) {
            return false;
        }
    }
    for (std::size_t i = 1; i < asks_.size(); ++i) {
        if (asks_[i - 1].price_ticks >= asks_[i].price_ticks) {
            return false;
        }
        if (asks_[i].qty_lots <= 0) {
            return false;
        }
    }
    if (!bids_.empty() && bids_.front().qty_lots <= 0) {
        return false;
    }
    if (!asks_.empty() && asks_.front().qty_lots <= 0) {
        return false;
    }
    L2Level b, a;
    if (best_bid(b) && best_ask(a) && b.price_ticks >= a.price_ticks) {
        return false;
    }
    return true;
}

}  // namespace kairos
