# Benchmarks and CI

Benchmarks are **not run in CI**. This is deliberate:

1. **Shared runners are noisy neighbors.** GitHub-hosted runners are VMs shared
   with unknown workloads. There is no core pinning, no control over frequency
   scaling, and no cache isolation — the same binary can report 2x different
   numbers on consecutive runs.
2. **Flaky gates teach people to ignore CI.** A benchmark that fails
   intermittently gets muted, and then it protects nothing.
3. **CI's job is correctness.** Build (clang + gcc, debug + release), unit /
   differential / randomized tests, and the sanitizer jobs are deterministic
   and belong in CI.

Benchmarks run **locally** via `./scripts/bench.sh` on a pinned core, following
`docs/benchmarks/METHODOLOGY.md`. Results are recorded per phase in
`docs/benchmarks/phaseN.md` with full hardware/OS/governor footers, so every
number is reproducible by the reader.
