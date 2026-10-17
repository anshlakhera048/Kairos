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


def run_tournament(binary, bots, seeds, duration, regime, out_path, quiet=False):
    """Run the tournament binary; return the parsed JSON results."""
    cmd = [
        binary,
        "--bots", ",".join(bots),
        "--seeds", str(seeds),
        "--duration", str(duration),
        "--regime", regime,
        "--out", out_path,
    ]
    if quiet:
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(
                f"kairos_tournament failed (exit {proc.returncode}):\n{proc.stderr}")
    else:
        proc = subprocess.run(cmd)
        if proc.returncode != 0:
            raise RuntimeError(f"kairos_tournament failed (exit {proc.returncode})")
    with open(out_path) as f:
        return json.load(f)


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


def cmd_arena(args):
    binary = find_tournament_bin()
    if binary is None:
        print("error: kairos_tournament binary not found.", file=sys.stderr)
        print("If you installed from source, build first: ./scripts/build.sh", file=sys.stderr)
        print("Or point $KAIROS_TOURNAMENT at the binary.", file=sys.stderr)
        return 2

    bots = [b.strip() for b in args.bots.split(",") if b.strip()]
    unknown = [b for b in bots if b not in BOTS]
    if unknown:
        print(f"error: unknown bot(s): {', '.join(unknown)} "
              f"(choose from {', '.join(BOTS)})", file=sys.stderr)
        return 2

    tmpdir = None
    try:
        if args.out:
            out_path = args.out
        else:
            tmpdir = tempfile.TemporaryDirectory()
            out_path = str(Path(tmpdir.name) / "results.json")

        print(f"running arena: bots={','.join(bots)} seeds={args.seeds} "
              f"duration={args.duration}s regime={args.regime}", flush=True)
        results = run_tournament(binary, bots, args.seeds, args.duration,
                                 args.regime, out_path, quiet=args.quiet)
        print()
        print_table(results, args.seeds, args.duration, args.regime)

        if args.out:
            print(f"\nraw results: {args.out}")
        if args.html:
            from .leaderboard import write_html
            write_html(results, args.html)
            print(f"leaderboard: {args.html}")
        return 0
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        if tmpdir is not None:
            tmpdir.cleanup()


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
                       help=f"comma-separated bot list (default: all). choices: {','.join(BOTS)}")
    arena.add_argument("--seeds", type=int, default=20)
    arena.add_argument("--duration", type=int, default=60, help="simulated seconds per seed")
    arena.add_argument("--regime", default="calm", choices=REGIMES)
    arena.add_argument("--out", default=None, help="write raw tournament JSON here")
    arena.add_argument("--html", default=None, help="write an HTML leaderboard here")
    arena.add_argument("--quiet", action="store_true", help="hide engine progress output")
    arena.set_defaults(func=cmd_arena)
    return ap


def _demo_args(a):
    return argparse.Namespace(
        bots="as,fixed,momentum", seeds=10, duration=30, regime="calm",
        out=None, html=a.html, quiet=a.quiet,
    )


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
