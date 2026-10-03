# Hosting Threat Model (Phase 4D)

Design doc only. This analyzes running strangers' code in a hosted arena.
The local harness (4A–4C) is the priority; hosting is a later stretch goal.

## Threat Model

**Adversary**: a participant submitting a bot to a public leaderboard.

**Assets**: host integrity, other participants' code/IP, leaderboard
integrity, host resources ($).

**Threats**:
1. **Host compromise**: bot escapes sandbox, reads host secrets, mines
   crypto, attacks network.
2. **IP theft**: bot reads other participants' code or the engine internals.
3. **Leaderboard gaming**: bot exploits harness bugs (lookahead, timing),
   submits many variants to overfit, or DoSes the runner.
4. **Resource abuse**: bot consumes excessive CPU/memory, starving others
   or inflating host costs.
5. **Data exfiltration**: bot phones home with market data or other
   participants' behavior.

## Sandboxing Options

| Approach | Isolation | Overhead | Complexity |
|---|---|---|---|
| Containers + seccomp | Process | Low | Medium |
| gVisor | Syscall | Medium | Medium |
| Firecracker microVMs | VM | Medium-High | High |
| WASM sandbox | Language | Low | High (need WASM toolchain) |

**No network** in all cases. **CPU/memory/time limits** via cgroups.
**Determinism**: the harness is already deterministic; the sandbox must
not introduce nondeterminism (e.g., via timing).

## Minimal Viable Approach (Recommendation)

For a solo developer: **containers with seccomp + no network + cgroups**.

- Use Docker/Podman with a seccomp profile blocking dangerous syscalls.
- Drop all capabilities (`--cap-drop=ALL`).
- Read-only filesystem except a scratch dir.
- No network (`--network=none`).
- CPU/memory limits via `--cpus` and `--memory`.
- Time limit via the harness (already have per-callback budget; add a
  total run timeout).
- Run as non-root user.

This stops 99% of attacks. It does not stop:
- Kernel exploits (need VM for that).
- Side-channel IP theft (timing) — acceptable for a leaderboard.

## What I Would NOT Attempt Solo

- **Building a custom sandbox**: use existing tools (containers, gVisor).
- **Firecracker at scale**: operational overhead is too high for one person.
- **Formal verification of the harness**: the anti-cheat is best-effort;
  a determined adversary will find bugs. The leaderboard is for fun,
  not for money.
- **Real-money trading**: the harness is a research tool. Connecting it
  to real markets requires audit, compliance, and risk controls that are
  out of scope.

## Abuse Cases to Handle

1. **Overfitting**: limit submissions per user per day; require a minimum
   number of seeds (the harness already uses 50).
2. **Sybil**: require GitHub OAuth; rate-limit accounts.
3. **DoS**: total timeout per run (e.g., 5 minutes); kill on exceed.
4. **Plagiarism**: hash submissions; flag duplicates (manual review).

## Conclusion

The local harness is solid. For hosting, start with containers+seccomp.
Do not build custom isolation. Do not promise security against nation-
states. The goal is a fun, fair leaderboard, not a bank vault.
