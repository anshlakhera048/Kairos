// Python bindings for the Kairos simulator (Phase 3B).
//
// Exposes the event-driven simulator to Python so researchers can write
// strategies in Python, run them on captured data, and get results as
// numpy-friendly arrays. The shared strategy adapter (PyStrategy,
// KairosPyContext) lives in py_strategy.hpp; arena bindings are registered
// from arena_bindings.cpp.

#include "py_strategy.hpp"

#include <pybind11/numpy.h>

// Registered in arena_bindings.cpp.
void init_arena_bindings(py::module_& m);


// ---------------------------------------------------------------------------
// Simulator driver.
// ---------------------------------------------------------------------------

struct PySimConfig {
    std::uint64_t feed_latency_ns = 0;
    std::uint64_t order_latency_ns = 0;
    std::string queue_model = "risk_averse";
    double p_ahead = 0.5;
    std::int64_t maker_fee_bp = 0;
    std::int64_t taker_fee_bp = 0;
    std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
};

template <typename Queue>
py::dict run_with_queue(const PySimConfig& cfg, const std::string& path,
                        py::object strategy,
                        std::uint64_t timer_interval_ns, Queue&& queue) {
    using Sim = Simulator<PyStrategy, ConstantLatency, ConstantLatency, Queue>;
    typename Sim::Config scfg;
    scfg.feed_lat = ConstantLatency{cfg.feed_latency_ns};
    scfg.order_lat = ConstantLatency{cfg.order_latency_ns};
    scfg.queue = std::forward<Queue>(queue);
    scfg.timer_interval_ns = timer_interval_ns;
    scfg.maker_fee_bp = cfg.maker_fee_bp;
    scfg.taker_fee_bp = cfg.taker_fee_bp;
    scfg.rng_seed = cfg.seed;

    Sim sim(scfg);
    PyStrategy py_strategy(strategy);

    typename Sim::Result res;
    {
        // Release the GIL: the C++ event loop (book updates, queue
        // tracking, latency sampling) runs without it. Callbacks
        // re-acquire as needed.
        py::gil_scoped_release release;
        res = sim.run({path}, py_strategy);
    }

    // Build numpy-friendly results (GIL held).
    const auto& fills = py_strategy.fills();
    const auto& equity = py_strategy.equity();

    py::dict fills_d;
    {
        const size_t n = fills.size();
        auto t = py::array_t<std::uint64_t>(n);
        auto px = py::array_t<std::int64_t>(n);
        auto qty = py::array_t<std::int64_t>(n);
        auto is_bid = py::array_t<bool>(n);
        auto t_p = t.mutable_data();
        auto px_p = px.mutable_data();
        auto qty_p = qty.mutable_data();
        auto bid_p = is_bid.mutable_data();
        for (size_t i = 0; i < n; ++i) {
            t_p[i] = fills[i].t_ns;
            px_p[i] = fills[i].price_ticks;
            qty_p[i] = fills[i].qty_lots;
            bid_p[i] = fills[i].is_bid;
        }
        fills_d["t_ns"] = std::move(t);
        fills_d["price_ticks"] = std::move(px);
        fills_d["qty_lots"] = std::move(qty);
        fills_d["is_bid"] = std::move(is_bid);
    }

    py::dict equity_d;
    {
        const size_t n = equity.size();
        auto t = py::array_t<std::uint64_t>(n);
        auto e = py::array_t<std::int64_t>(n);
        auto t_p = t.mutable_data();
        auto e_p = e.mutable_data();
        for (size_t i = 0; i < n; ++i) {
            t_p[i] = equity[i].t_ns;
            e_p[i] = equity[i].equity_lots;
        }
        equity_d["t_ns"] = std::move(t);
        equity_d["equity_lots"] = std::move(e);
    }

    py::dict stats;
    stats["market_events"] = res.market_events;
    stats["fills"] = res.fills;
    stats["acks"] = res.acks;
    stats["rejects"] = res.rejects;
    stats["cancels"] = res.cancels;
    stats["final_position_lots"] = res.account.position_lots;
    stats["final_cash_lots"] = res.account.cash_lots;

    py::dict out;
    out["fills"] = std::move(fills_d);
    out["equity"] = std::move(equity_d);
    out["stats"] = std::move(stats);
    return out;
}

py::dict run_simulation(const PySimConfig& cfg, const std::string& path,
                        py::object strategy,
                        std::uint64_t timer_interval_ns) {
    if (cfg.queue_model == "naive") {
        return run_with_queue(cfg, path, strategy, timer_interval_ns,
                              NaiveQueue{});
    } else if (cfg.queue_model == "probabilistic") {
        return run_with_queue(cfg, path, strategy, timer_interval_ns,
                              ProbabilisticQueue(cfg.p_ahead, cfg.seed));
    } else if (cfg.queue_model == "risk_averse") {
        return run_with_queue(cfg, path, strategy, timer_interval_ns,
                              RiskAverseQueue{});
    }
    throw std::invalid_argument("unknown queue_model: " + cfg.queue_model +
                                " (expected 'naive', 'risk_averse', or"
                                " 'probabilistic')");
}

// ---------------------------------------------------------------------------
// Module.
// ---------------------------------------------------------------------------

PYBIND11_MODULE(kairos_py, m) {
    m.doc() = "Kairos simulator + arena Python bindings";

    // Arena bindings (arena_bindings.cpp).
    init_arena_bindings(m);

    py::class_<KairosPyContext>(m, "Context")
        .def_readonly("now_ns", &KairosPyContext::now_ns)
        .def_readonly("has_bid", &KairosPyContext::has_bid)
        .def_readonly("bid_px", &KairosPyContext::bid_px)
        .def_readonly("bid_qty", &KairosPyContext::bid_qty)
        .def_readonly("has_ask", &KairosPyContext::has_ask)
        .def_readonly("ask_px", &KairosPyContext::ask_px)
        .def_readonly("ask_qty", &KairosPyContext::ask_qty)
        .def_readonly("position_lots", &KairosPyContext::position_lots)
        .def_readonly("cash_lots", &KairosPyContext::cash_lots)
        .def("best_bid", [](const KairosPyContext& c) -> py::object {
            if (!c.has_bid) return py::none();
            return py::make_tuple(c.bid_px, c.bid_qty);
        })
        .def("best_ask", [](const KairosPyContext& c) -> py::object {
            if (!c.has_ask) return py::none();
            return py::make_tuple(c.ask_px, c.ask_qty);
        })
        .def("equity_lots", [](const KairosPyContext& c, std::int64_t mark_px) {
            return c.equity_lots(mark_px);
        })
        .def("send_limit",
             [](const KairosPyContext& c, const std::string& side,
                std::int64_t price_ticks, std::int64_t qty_lots,
                bool post_only) {
                 return c.send_limit(side, price_ticks, qty_lots, post_only);
             },
             py::arg("side"), py::arg("price_ticks"), py::arg("qty_lots"),
             py::arg("post_only") = true)
        .def("cancel", [](const KairosPyContext& c, std::uint64_t id) {
            c.cancel(id);
        })
        .def("modify",
             [](const KairosPyContext& c, std::uint64_t id, std::int64_t px,
                std::int64_t qty) { return c.modify(id, px, qty); });

    py::class_<PySimConfig>(m, "SimConfig")
        .def(py::init<>())
        .def_readwrite("feed_latency_ns", &PySimConfig::feed_latency_ns)
        .def_readwrite("order_latency_ns", &PySimConfig::order_latency_ns)
        .def_readwrite("queue_model", &PySimConfig::queue_model)
        .def_readwrite("p_ahead", &PySimConfig::p_ahead)
        .def_readwrite("maker_fee_bp", &PySimConfig::maker_fee_bp)
        .def_readwrite("taker_fee_bp", &PySimConfig::taker_fee_bp)
        .def_readwrite("seed", &PySimConfig::seed);

    m.def("run", &run_simulation, py::arg("config"), py::arg("capture_path"),
          py::arg("strategy"), py::arg("timer_interval_ns") = 0,
          "Run the simulator on a capture file with a Python strategy.\n\n"
          "Returns a dict with 'fills', 'equity' (dicts of numpy arrays),\n"
          "and 'stats' (dict of counters).");
}
