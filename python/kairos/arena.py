"""Python-side tournament driver.

Runs Python bots on the deterministic C++ arena via the kairos_py
extension. Result dicts use the same keys as the kairos_tournament JSON
output, so the leaderboard scoring works unchanged.

    from kairos.arena import run_tournament
    from kairos.agents import DebateTrader

    results = run_tournament([("debate", DebateTrader)], seeds=5,
                             duration_s=30, regime="calm")
"""

from . import kairos_py

FIRST_SEED = 1000  # matches the C++ tournament driver


def run_tournament(specs, seeds=20, duration_s=60, regime="calm",
                   callback_budget_s=180.0,
                   timer_interval_ns=500_000_000, quiet=False,
                   progress=None):
    """Run each (name, bot_factory) on each seed. Returns result dicts.

    bot_factory is a zero-arg callable returning a fresh bot instance per
    seed (fresh state per seed, like the C++ tournament).
    progress(name, seed_index, seeds) is called before each run, if given.
    """
    results = []
    budget_ns = int(callback_budget_s * 1e9)
    for name, factory in specs:
        for s in range(seeds):
            seed = FIRST_SEED + s
            if progress is not None:
                progress(name, s, seeds)
            elif not quiet:
                print(f"  py-bot={name} seed={seed} ...", flush=True)
            r = kairos_py.arena_run(
                factory, name, regime=regime, seed=seed,
                duration_s=duration_s, callback_budget_ns=budget_ns,
                timer_interval_ns=timer_interval_ns)
            results.append(r)
            if not quiet and progress is None:
                print(f"  py-bot={name} seed={seed} fills={r['fills']} "
                      f"pnl={r['pnl_lots']}"
                      f"{' DISQUALIFIED' if r['disqualified'] else ''}")
    return results
