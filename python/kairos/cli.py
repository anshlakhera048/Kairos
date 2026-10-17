#!/usr/bin/env python3
"""The `kairos` command-line interface.

Front door to the Kairos matching engine. Subcommands:

    kairos demo    A ~30-second arena run. The two-minute wow.
    kairos arena   Full tournament: bots compete on the deterministic C++
                   matching engine; results print as a ranked table.

The arena itself runs in the compiled `kairos_tournament` binary shipped
inside the wheel (or built from source); this CLI drives it, parses its JSON,
and presents the leaderboard.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from importlib.metadata import version as _pkg_version

    __version__ = _pkg_version("kairos-engine")
except Exception:
    __version__ = "0.1.0"  # fallback: source checkout without install

BOTS = ["as", "fixed", "momentum", "random", "cheater"]
REGIMES = ["calm", "volatile", "informed_heavy"]


def _use_color():
    return sys.stdout.isatty() and os.environ.get("NO_COLOR") is None


def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if _use_color() else text


def find_tournament_bin():
    """Locate the kairos_tournament binary.

    Order: $KAIROS_TOURNAMENT override, wheel install (kairos/bin/),
    PATH, then common in-source build trees for developer checkouts.
    """
    env = os.environ.get("KAIROS_TOURNAMENT")
    if env and Path(env).is_file():
        return env

    exe = "kairos_tournament.exe" if platform.system() == "Windows" else "kairos_tournament"

    pkg_bin = Path(__file__).resolve().parent / "bin" / exe
    if pkg_bin.is_file():
        return str(pkg_bin)

    on_path = shutil.which("kairos_tournament")
    if on_path:
        return on_path

    repo_root = Path(__file__).resolve().parent.parent.parent
    for build_dir in ("build", "build-release", "build-debug"):
        cand = repo_root / build_dir / "tools" / exe
        if cand.is_file():
            return str(cand)

    return None


def print_table(scores, seeds, duration, regime):
    """Print the ranked leaderboard. Scores come from kairos.leaderboard."""
    from .leaderboard import compute_scores  # local import: keep cli importable

    if isinstance(scores, list) and scores and "bot" in scores[0] and "total" not in scores[0]:
        scores = compute_scores(scores)

    header = (f"Kairos Arena — {seeds} seeds x {duration}s "
              f"\u00b7 regime={regime} \u00b7 deterministic C++ engine")
    print(_c("1", header))
    print(f"{_c('2', '#'):>3}  {_c('1', 'bot'):<10} {_c('1', 'score'):>8} "
          f"{_c('1', 'mean PnL($)'):>12} {_c('1', 'fills'):>8} "
          f"{_c('1', 'seeds'):>6} {_c('1', 'dq'):>3}")
    for i, s in enumerate(scores, 1):
        row = (f"{i:>3}  {s['bot']:<10} {s['total']:>8.2f} "
               f"{s['mean_pnl']:>12.2f} {s['total_fills']:>8} "
               f"{s['n_seeds']:>6} {s['disqualified']:>3}")
        if i == 1:
            row = _c("32", row)  # winner in green
        elif s["disqualified"]:
            row = _c("31", row)  # disqualified in red
        print(row)
    print(_c("2", "score = mean PnL - inventory penalty + risk-adjusted term"))


def load_py_bot(spec):
    """Load a Python bot factory from a 'path/to/bot.py:Factory' spec."""
    path_s, sep, attr = spec.partition(":")
    if not sep or not attr:
        raise ValueError(
            f"bad --py-bot spec {spec!r}: expected path/to/bot.py:FactoryName")
    p = Path(path_s)
    if not p.is_file():
        raise ValueError(f"bot file not found: {path_s}")
    mod_name = "kairos_user_bot_" + hashlib.md5(
        str(p.resolve()).encode()).hexdigest()[:8]
    mod_spec = importlib.util.spec_from_file_location(mod_name, p)
    if mod_spec is None or mod_spec.loader is None:
        raise ValueError(f"cannot load bot file: {path_s}")
    mod = importlib.util.module_from_spec(mod_spec)
    sys.modules[mod_name] = mod
    mod_spec.loader.exec_module(mod)
    factory = getattr(mod, attr, None)
    if not callable(factory):
        raise ValueError(
            f"{path_s} has no callable {attr!r} (expected a bot class or factory)")
    return attr, factory


def _run_tournament(cxx_bots, py_specs, seeds, duration, regime,
                    budget_s, quiet, out_path):
    """Run the C++ and/or Python tournaments. Returns the results list."""
    results = []
    if cxx_bots:
        binary = find_tournament_bin()
        if binary is None:
            raise RuntimeError(
                "kairos_tournament binary not found. "
                "If you installed from source, build first: ./scripts/build.sh "
                "Or point $KAIROS_TOURNAMENT at the binary.")
        print(f"running arena (C++): bots={','.join(cxx_bots)} "
              f"seeds={seeds} duration={duration}s "
              f"regime={regime}", flush=True)
        cmd = [binary, "--bots", ",".join(cxx_bots),
               "--seeds", str(seeds), "--duration", str(duration),
               "--regime", regime, "--out", out_path]
        if quiet:
            proc = subprocess.run(cmd, capture_output=True, text=True)
            if proc.returncode != 0:
                raise RuntimeError(
                    f"kairos_tournament failed (exit {proc.returncode}):\n"
                    f"{proc.stderr}")
        else:
            proc = subprocess.run(cmd)
            if proc.returncode != 0:
                raise RuntimeError(
                    f"kairos_tournament failed (exit {proc.returncode})")
        with open(out_path) as f:
            results.extend(json.load(f))

    if py_specs:
        from .arena import run_tournament as run_py_tournament
        print(f"running arena (Python): bots="
              f"{','.join(n for n, _ in py_specs)} seeds={seeds} "
              f"duration={duration}s regime={regime} "
              f"budget={budget_s}s/callback", flush=True)
        results.extend(run_py_tournament(
            py_specs, seeds=seeds, duration_s=duration,
            regime=regime, callback_budget_s=budget_s,
            quiet=quiet))
    return results


def _present_results(results, seeds, duration, regime, out_path,
                     html_path=None, announced_out=None):
    print()
    print_table(results, seeds, duration, regime)
    with open(out_path, "w") as f:
        json.dump(results, f)
    if announced_out:
        print(f"\nraw results: {announced_out}")
    if html_path:
        from .leaderboard import write_html
        write_html(results, html_path)
        print(f"leaderboard: {html_path}")


def cmd_arena(args):
    bots_arg = (args.bots or "").strip()
    cxx_bots = [] if bots_arg.lower() in ("", "none") else [
        b.strip() for b in bots_arg.split(",") if b.strip()]
    unknown = [b for b in cxx_bots if b not in BOTS]
    if unknown:
        print(f"error: unknown bot(s): {', '.join(unknown)} "
              f"(choose from {', '.join(BOTS)})", file=sys.stderr)
        return 2

    py_specs = []
    for spec in args.py_bot or []:
        try:
            py_specs.append(load_py_bot(spec))
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            return 2

    if not cxx_bots and not py_specs:
        print("error: nothing to run (--bots none and no --py-bot given)",
              file=sys.stderr)
        return 2

    tmpdir = None
    try:
        if args.out:
            out_path = args.out
        else:
            tmpdir = tempfile.TemporaryDirectory()
            out_path = str(Path(tmpdir.name) / "results.json")

        results = _run_tournament(cxx_bots, py_specs, args.seeds,
                                  args.duration, args.regime, args.budget_s,
                                  args.quiet, out_path)
        _present_results(results, args.seeds, args.duration, args.regime,
                         out_path, html_path=args.html,
                         announced_out=args.out)

        if args.record is not None:
            from .seasons import record, default_ledger_path
            ledger = args.ledger or default_ledger_path()
            season_id = args.record if args.record else "open"
            n = record(ledger, season_id, results)
            print(f"recorded {n} results to {ledger} (season: {season_id})")
        return 0
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        if tmpdir is not None:
            tmpdir.cleanup()


def cmd_leaderboard(args):
    from .seasons import (default_ledger_path, load_season, read_records,
                          season_scores)
    ledger = args.ledger or default_ledger_path()
    records = read_records(ledger, args.season)
    if not records:
        scope = f" for '{args.season}'" if args.season else ""
        print(f"no records{scope} in {ledger}")
        print("run one: kairos season run season-01")
        return 1
    scoring = {"lambda_inv": 1.0, "lambda_risk": 1.0, "spread_cost": 0.01}
    title = "Kairos Bot Leaderboard"
    if args.season:
        try:
            scoring = load_season(args.season)["scoring"]
            title = f"Kairos Leaderboard — {load_season(args.season)['name']}"
        except ValueError:
            title = f"Kairos Leaderboard — {args.season}"
    scores = season_scores(records, scoring)
    print(_c("1", title))
    print(f"{'rank':>4}  {'bot':<14} {'score':>9} {'mean PnL($)':>11} "
          f"{'seeds':>6} {'dq':>3}")
    for i, s in enumerate(scores, 1):
        mark = _c("33", " *") if s["disqualified"] else ""
        print(f"{i:>4}  {s['bot']:<14} {s['total']:>9.2f} "
              f"{s['mean_pnl']:>11.2f} {s['n_seeds']:>6} "
              f"{s['disqualified']:>3}{mark}")
    if any(s["disqualified"] for s in scores):
        print(_c("33", " * disqualified in one or more seeds"))
    if args.html:
        from .leaderboard import render_html
        Path(args.html).write_text(render_html(scores, title))
        print(f"leaderboard: {args.html}")
    return 0


def cmd_season_list(args):
    from .seasons import list_seasons
    seasons = list_seasons()
    if not seasons:
        print("no published seasons yet")
        return 1
    for cfg in seasons:
        print(f"{cfg['id']}: {cfg['name']} "
              f"(bots={','.join(cfg['bots'])}, seeds={cfg['seeds']}, "
              f"{cfg['duration_s']}s, {cfg['regime']})")
    return 0


def cmd_season_run(args):
    from .seasons import default_ledger_path, load_season, record
    try:
        cfg = load_season(args.season_id)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    print(_c("1", f"Season {cfg['id']}: {cfg['name']}"))
    if cfg.get("rules"):
        print(_c("2", f"rules: {cfg['rules']}"))
    tmpdir = tempfile.TemporaryDirectory()
    try:
        out_path = str(Path(tmpdir.name) / "season_results.json")
        ns = argparse.Namespace(
            bots=",".join(cfg["bots"]), py_bot=[], seeds=cfg["seeds"],
            duration=cfg["duration_s"], regime=cfg["regime"], budget_s=180,
            quiet=False, out=out_path, html=args.html, record=None,
            ledger=None)
        rc = cmd_arena(ns)
        if rc != 0:
            return rc
        with open(out_path) as f:
            results = json.load(f)
        ledger = args.ledger or default_ledger_path()
        n = record(ledger, cfg["id"], results)
        print(f"recorded {n} results to {ledger}")
        return 0
    finally:
        tmpdir.cleanup()


def cmd_realism_gap(args):
    from .realism import run_gap, render_svg

    print(_c("1", "Realism gap — same strategy, same data, different fill honesty"))
    if args.capture:
        print(f"capture: {args.capture}")
    else:
        print(f"generating {args.seconds:g}s synthetic L2 capture "
              f"(seed {args.seed}) ...", flush=True)
    rows, meta = run_gap(
        capture_path=args.capture, seconds=args.seconds, seed=args.seed,
        progress=lambda lv: print(f"  {lv} ...", flush=True))
    print(f"{_c('1', 'level'):>5}  {_c('1', 'description'):<42} "
          f"{_c('1', 'PnL'):>9} {_c('1', 'fills'):>7} {_c('1', 'fill rate'):>9}")
    for r in rows:
        line = (f"{r['level']:>5}  {r['label']:<42} "
                f"${r['pnl_dollars']:>8.2f} {r['fills']:>7} "
                f"{100 * r['fill_rate']:>8.1f}%")
        if r["level"] == "L1":
            line = _c("31", line)  # naive in red
        elif r["level"] == "L3":
            line = _c("32", line)  # honest in green
        print(line)
    print()
    print(_c("1", f"-> naive fill-at-touch overstates PnL by "
                  f"{meta['overstatement_pct']:.1f}% "
                  f"vs the risk-averse queue model"))
    print(_c("2", "synthetic data: relative gap is the finding, "
                  "absolute PnL is not tradable"))

    if args.out:
        import json as _json
        with open(args.out, "w") as f:
            _json.dump({"meta": meta, "rows": rows}, f, indent=2)
        print(f"results: {args.out}")
    if args.chart:
        with open(args.chart, "w") as f:
            f.write(render_svg(rows, meta))
        print(f"chart: {args.chart}")
    return 0


def build_parser():
    ap = argparse.ArgumentParser(
        prog="kairos",
        description="Kairos: deterministic matching engine + market-microstructure arena.")
    ap.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    demo = sub.add_parser("demo", help="30-second arena run. The two-minute wow.")
    demo.add_argument("--html", default=None, help="also write an HTML leaderboard")
    demo.add_argument("--quiet", action="store_true")
    demo.set_defaults(func=lambda a: cmd_arena(_demo_args(a)))

    arena = sub.add_parser("arena", help="run the tournament arena")
    arena.add_argument("--bots", default=",".join(BOTS),
                       help=f"comma-separated C++ bot list (default: all; "
                            f"'none' for no C++ bots). choices: {','.join(BOTS)}")
    arena.add_argument("--py-bot", action="append", default=[],
                       help="Python bot as path/to/bot.py:FactoryName "
                            "(repeatable; runs on the same seeds)")
    arena.add_argument("--budget-s", type=float, default=180.0,
                       help="per-callback wall-clock thinking budget in "
                            "seconds for Python bots (default: 180)")
    arena.add_argument("--seeds", type=int, default=20)
    arena.add_argument("--duration", type=int, default=60, help="simulated seconds per seed")
    arena.add_argument("--regime", default="calm", choices=REGIMES)
    arena.add_argument("--out", default=None, help="write raw tournament JSON here")
    arena.add_argument("--html", default=None, help="write an HTML leaderboard here")
    arena.add_argument("--quiet", action="store_true", help="hide engine progress output")
    arena.add_argument("--record", nargs="?", const="", metavar="SEASON",
                       help="append results to the leaderboard ledger "
                            "(default season: open)")
    arena.add_argument("--ledger", default=None,
                       help="ledger file (default: repo data/leaderboard/ledger.jsonl)")
    arena.set_defaults(func=cmd_arena)

    rg = sub.add_parser("realism-gap",
                        help="naive vs honest fill PnL demo")
    rg.add_argument("--seconds", type=float, default=600,
                    help="synthetic capture length in seconds (default: 600)")
    rg.add_argument("--seed", type=int, default=7,
                    help="synth capture seed (default: 7)")
    rg.add_argument("--capture", default=None,
                    help="use an existing .kai capture instead of generating one")
    rg.add_argument("--out", default=None, help="write results JSON here")
    rg.add_argument("--chart", default=None,
                    help="write an SVG equity-curve chart here")
    rg.set_defaults(func=cmd_realism_gap)

    lb = sub.add_parser("leaderboard",
                        help="show the recorded bot leaderboard")
    lb.add_argument("--season", default=None,
                    help="season id (default: all recorded results)")
    lb.add_argument("--ledger", default=None,
                    help="ledger file (default: repo data/leaderboard/ledger.jsonl)")
    lb.add_argument("--html", default=None,
                    help="write a self-contained HTML leaderboard here")
    lb.set_defaults(func=cmd_leaderboard)

    sea = sub.add_parser("season", help="published bot competitions")
    sea_sub = sea.add_subparsers(dest="season_cmd", required=True)
    sr = sea_sub.add_parser("run", help="run a published season")
    sr.add_argument("season_id", help="e.g. season-01")
    sr.add_argument("--ledger", default=None,
                    help="ledger file (default: repo data/leaderboard/ledger.jsonl)")
    sr.add_argument("--html", default=None,
                    help="write a self-contained HTML leaderboard here")
    sr.set_defaults(func=cmd_season_run)
    sl = sea_sub.add_parser("list", help="list published seasons")
    sl.set_defaults(func=cmd_season_list)
    return ap


def _demo_args(a):
    return argparse.Namespace(
        bots="as,fixed,momentum", py_bot=[], budget_s=180.0,
        seeds=10, duration=30, regime="calm",
        out=None, html=a.html, quiet=a.quiet,
        record=None, ledger=None,
    )


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
