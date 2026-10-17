// Python bindings for the Kairos tournament arena (Phase 5).
//
// Lets Python bots compete on the deterministic C++ matching engine.
// Reuses PyStrategy from py_strategy.hpp: the adapter is generic over the
// Context type, and the arena's Context satisfies the same interface as
// the simulator's, so no adapter changes were needed.
//
// Fairness note: the arena's per-callback wall-clock budget applies to
// Python bots too. Python/LLM bots should pass a generous
// callback_budget_ns — the market runs in logical time, so thinking
// longer buys no market advantage; the budget only guards against hangs.
// A fresh bot instance is created per seed (via bot_factory), matching
// the C++ tournament's fresh-state-per-seed discipline.

#include "py_strategy.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "kairos/arena.hpp"
#include "kairos/flow.hpp"

using namespace kairos;
using namespace kairos::arena;

namespace {

Regime regime_for(const std::string& name, std::uint64_t seed) {
    if (name == "calm") return regime_calm(seed);
    if (name == "volatile") return regime_volatile(seed);
    if (name == "informed_heavy") return regime_informed_heavy(seed);
    throw std::invalid_argument(
        "unknown regime '" + name + "' (expected calm|volatile|informed_heavy)");
}

py::dict bot_result_to_dict(const BotResult& r) {
    py::dict d;
    // Keys intentionally match the kairos_tournament JSON schema so the
    // existing leaderboard scoring works unchanged.
    d["bot"] = r.bot_name;
    d["seed"] = r.seed;
    d["fills"] = r.fills;
    d["acks"] = r.acks;
    d["rejects"] = r.rejects;
    d["position"] = r.final_position_lots;
    d["pnl_lots"] = r.realized_pnl_lots;
    d["disqualified"] = r.disqualified;
    d["dq_reason"] = r.dq_reason;
    return d;
}

void add_default_flow(Arena<PyStrategy>& arena, const Regime& regime) {
    arena.add_generator(std::make_unique<NoiseTrader>(regime.noise, 10));
    arena.add_generator(
        std::make_unique<InformedTrader>(regime.informed, 10000, 11));
    FixedSpreadConfig fc;
    arena.add_generator(std::make_unique<FixedSpreadMaker>(fc, 12));
    MomentumConfig mc;
    arena.add_generator(std::make_unique<MomentumTrader>(mc, 13));
}

py::dict arena_run(py::object bot_factory, const std::string& bot_name,
                   const std::string& regime_name, std::uint64_t seed,
                   std::uint64_t duration_s, std::uint64_t callback_budget_ns,
                   std::uint64_t timer_interval_ns) {
    ArenaConfig cfg;
    cfg.seed = seed;
    cfg.duration_ns = duration_s * 1'000'000'000ULL;
    cfg.callback_time_budget_ns = callback_budget_ns;
    cfg.timer_interval_ns = timer_interval_ns;

    const Regime regime = regime_for(regime_name, seed);
    Arena<PyStrategy> arena(cfg);
    add_default_flow(arena, regime);

    py::object py_bot = bot_factory();  // fresh state per seed
    PyStrategy bot(py_bot);

    BotResult res;
    {
        // Release the GIL: the C++ event loop runs without it; Python
        // callbacks re-acquire via PyStrategy.
        py::gil_scoped_release release;
        res = arena.run(bot, bot_name);
    }
    return bot_result_to_dict(res);
}

}  // namespace

// Registered from the PYBIND11_MODULE in bindings.cpp.
void init_arena_bindings(py::module_& m) {
    m.def(
        "arena_run", &arena_run, py::arg("bot_factory"), py::arg("bot_name"),
        py::arg("regime") = "calm", py::arg("seed") = 1000,
        py::arg("duration_s") = 60,
        py::arg("callback_budget_ns") = 5'000'000'000ULL,
        py::arg("timer_interval_ns") = 500'000'000ULL,
        "Run one Python bot on one arena seed.\n\n"
        "bot_factory is a zero-arg callable returning a fresh bot instance\n"
        "(fresh state per seed, like the C++ tournament). Returns a dict\n"
        "with the same keys as the kairos_tournament JSON output.\n\n"
        "callback_budget_ns is the per-callback wall-clock thinking budget\n"
        "(anti-cheat). Python/LLM bots need seconds, not the 5ms C++\n"
        "default: the market runs in logical time, so a larger budget buys\n"
        "no market advantage.");
}
