#pragma once

// Shared Python-strategy adapter, used by the simulator bindings
// (bindings.cpp) and the arena bindings (arena_bindings.cpp).
//
// PyStrategy is a C++ adapter that satisfies the Strategy concept
// (simulator.hpp) and forwards the seven callbacks to a Python object.
// It is generic over the Context type: make_py_context works with any
// context exposing now(), book(), account(), send_limit(), cancel(),
// modify() — which is exactly what the arena's Context provides, so
// Python bots run in the arena with no adapter changes.
//
// Design notes:
//   - The C++ fast path is untouched; the Python path pays GIL +
//     boundary cost per callback (measured in docs/python-overhead.md).
//   - Context is passed as a value-type KairosPyContext with
//     std::function closures for actions. Closures capture the
//     context by reference; safe because the Python call is
//     synchronous (the closure cannot outlive the callback).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "kairos/account.hpp"
#include "kairos/fill_model.hpp"
#include "kairos/l2_book.hpp"
#include "kairos/latency.hpp"
#include "kairos/simulator.hpp"

namespace py = pybind11;
using namespace kairos;

// ---------------------------------------------------------------------------
// KairosPyContext: what the Python strategy sees.
// ---------------------------------------------------------------------------

struct KairosPyContext {
    std::uint64_t now_ns = 0;
    bool has_bid = false;
    std::int64_t bid_px = 0;
    std::int64_t bid_qty = 0;
    bool has_ask = false;
    std::int64_t ask_px = 0;
    std::int64_t ask_qty = 0;
    std::int64_t position_lots = 0;
    std::int64_t cash_lots = 0;

    std::function<std::uint64_t(const std::string&, std::int64_t,
                                 std::int64_t, bool)>
        send_limit;
    std::function<void(std::uint64_t)> cancel;
    std::function<std::uint64_t(std::uint64_t, std::int64_t, std::int64_t)>
        modify;
    std::function<std::int64_t(std::int64_t)> equity_lots;
};

template <typename Ctx>
KairosPyContext make_py_context(Ctx& ctx) {
    KairosPyContext p;
    p.now_ns = ctx.now();
    L2Level lvl;
    if (ctx.book().best_bid(lvl)) {
        p.has_bid = true;
        p.bid_px = lvl.price_ticks;
        p.bid_qty = lvl.qty_lots;
    }
    if (ctx.book().best_ask(lvl)) {
        p.has_ask = true;
        p.ask_px = lvl.price_ticks;
        p.ask_qty = lvl.qty_lots;
    }
    p.position_lots = ctx.account().position_lots;
    p.cash_lots = ctx.account().cash_lots;

    // Closures capture ctx by reference; safe because the Python call is
    // synchronous (the closure cannot outlive the callback).
    p.send_limit = [&ctx](const std::string& side, std::int64_t px,
                          std::int64_t qty, bool post_only) {
        const SimSide s =
            (side == "bid" || side == "BID") ? SimSide::Bid : SimSide::Ask;
        return ctx.send_limit(s, px, qty, post_only);
    };
    p.cancel = [&ctx](std::uint64_t id) { ctx.cancel(id); };
    p.modify = [&ctx](std::uint64_t id, std::int64_t px, std::int64_t qty) {
        return ctx.modify(id, px, qty);
    };
    p.equity_lots = [&ctx](std::int64_t mark_px) {
        return ctx.account().equity_lots(mark_px);
    };
    return p;
}

// ---------------------------------------------------------------------------
// PyStrategy: C++ adapter satisfying the Strategy concept.
// ---------------------------------------------------------------------------

struct FillRecord {
    std::uint64_t t_ns = 0;
    std::int64_t price_ticks = 0;
    std::int64_t qty_lots = 0;
    bool is_bid = false;
};

struct EquityPoint {
    std::uint64_t t_ns = 0;
    std::int64_t equity_lots = 0;
};

class PyStrategy {
public:
    explicit PyStrategy(py::object obj) : py_obj_(std::move(obj)) {}

    template <typename Ctx>
    void on_book_update(Ctx& ctx) {
        py::gil_scoped_acquire gil;
        py_obj_.attr("on_book_update")(make_py_context(ctx));
    }

    template <typename Ctx>
    void on_trade(Ctx& ctx, const TradeInfo& t) {
        py::gil_scoped_acquire gil;
        py::dict d;
        d["exchange_ts_ns"] = t.exchange_ts_ns;
        d["price_ticks"] = t.price_ticks;
        d["qty_lots"] = t.qty_lots;
        d["taker_is_bid"] = t.taker_is_bid;
        py_obj_.attr("on_trade")(make_py_context(ctx), d);
    }

    template <typename Ctx>
    void on_own_fill(Ctx& ctx, const SimFill& f) {
        {
            py::gil_scoped_acquire gil;
            py::dict d;
            d["order_id"] = f.order_id;
            d["side"] = (f.side == SimSide::Bid) ? "bid" : "ask";
            d["price_ticks"] = f.price_ticks;
            d["qty_lots"] = f.qty_lots;
            d["is_maker"] = f.is_maker;
            py_obj_.attr("on_own_fill")(make_py_context(ctx), d);
        }
        // Record for the results (GIL not needed for the vector).
        FillRecord r;
        r.t_ns = ctx.now();
        r.price_ticks = f.price_ticks;
        r.qty_lots = f.qty_lots;
        r.is_bid = (f.side == SimSide::Bid);
        fills_.push_back(r);
    }

    template <typename Ctx>
    void on_ack(Ctx& ctx, const AckInfo& a) {
        py::gil_scoped_acquire gil;
        py::dict d;
        d["order_id"] = a.order_id;
        d["effective_exchange_ts"] = a.effective_exchange_ts;
        py_obj_.attr("on_ack")(make_py_context(ctx), d);
    }

    template <typename Ctx>
    void on_reject(Ctx& ctx, std::uint64_t id) {
        py::gil_scoped_acquire gil;
        py_obj_.attr("on_reject")(make_py_context(ctx), id);
    }

    template <typename Ctx>
    void on_cancel(Ctx& ctx, std::uint64_t id) {
        py::gil_scoped_acquire gil;
        py_obj_.attr("on_cancel")(make_py_context(ctx), id);
    }

    template <typename Ctx>
    void on_timer(Ctx& ctx) {
        {
            py::gil_scoped_acquire gil;
            py_obj_.attr("on_timer")(make_py_context(ctx));
        }
        // Sample equity at the mid (for the results).
        L2Level bb, ba;
        if (ctx.book().best_bid(bb) && ctx.book().best_ask(ba)) {
            const std::int64_t mid =
                bb.price_ticks + (ba.price_ticks - bb.price_ticks) / 2;
            equity_.push_back({ctx.now(), ctx.account().equity_lots(mid)});
        }
    }

    const std::vector<FillRecord>& fills() const { return fills_; }
    const std::vector<EquityPoint>& equity() const { return equity_; }

private:
    py::object py_obj_;
    std::vector<FillRecord> fills_;
    std::vector<EquityPoint> equity_;
};
