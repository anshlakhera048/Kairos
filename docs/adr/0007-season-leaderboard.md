# ADR 0007 — Season leaderboard scaffolding

Date: 2026-10-18
Status: accepted

## Context

A public, persistent bot leaderboard is the contribution flywheel: it
gives bot authors a reason to compete, a scoreboard to share, and the
project a living hall of fame. But a hosted service is out of scope —
the scaffolding must work with git + the CLI alone.

## Decision

1. **Seasons are published configs** (`data/seasons/season-01.json`):
   regime, duration, seed count, eligible bots, scoring weights, budgets,
   and the rules text. Anyone can reproduce a season exactly.
2. **The ledger is append-only JSONL** (`data/leaderboard/ledger.jsonl`
   in the repo for official seasons, `~/.kairos/ledger.jsonl` for ad-hoc
   runs): one object per (bot, seed) with season_id and recorded_at.
   Official season results are committed, so the leaderboard is public
   by construction.
3. **CLI**: `kairos season run <id>` runs the published config and
   records it; `kairos season list` lists seasons; `kairos leaderboard
   [--season <id>] [--html]` shows the ranked table; `kairos arena
   --record [season]` records ad-hoc runs under "open" or a named season.
4. **Scoring is the arena scoring** (mean PnL − inventory penalty +
   Sharpe-like term) with the season's weights — one canonical formula.

## Consequences

- Season 1 (built-in bots, 20 seeds × 60s, calm) ran as the inaugural
  official result; the table is in the README hall of fame.
- Community seasons work the same way: publish a config, run it,
  commit the ledger. No backend to operate.
- Anti-gaming is social, not technical, at this stage: official results
  are maintainer-run and the config is reproducible, so anyone can
  verify a claimed score.

## Alternatives considered

- **Hosted leaderboard API** — rejected: operating a service is a
  different project; git-committed ledgers are public, versioned, and
  free.
- **Elo/Glicko ratings** — deferred: the arena score already balances
  PnL, inventory, and risk; ratings can layer on later once community
  seasons produce cross-season data.
