# Benchmark Methodology

Every number we publish must be reproducible. Follow this procedure; record any
deviation alongside the results.

## 1. Pinned core

Run the benchmark pinned to one isolated core, e.g.:

```bash
taskset -c 2 ./build/bench/bench/kairos_bench
```

(`scripts/bench.sh` does this.) Never benchmark on an unpinned process — the
scheduler migrating the thread between cores invalidates cache-warmth
assumptions.

## 2. Warmup

Discard the first N iterations (Google Benchmark does this by default; our
custom harness must do it explicitly). Caches, branch predictors and the CPU
frontend need steady state before measurement starts.

## 3. CPU isolation

Best effort, in descending order of rigor:

1. Boot with `isolcpus=` / `nohz_full=` for the benchmark core and move IRQs
   away (`/proc/irq/*/smp_affinity`).
2. At minimum: `nice -n -20`, pinned core, machine otherwise idle, no browser /
   video calls / builds running concurrently.

Record which level was achieved with the results.

## 4. Frequency scaling

- Set the `performance` governor: `cpupower frequency-set -g performance`.
- Note whether turbo/boost was disabled in BIOS (preferred for
  comparability) or left on (note it).
- Record `lscpu` MHz range with the results.

## 5. Coordinated omission

The classic trap: a load generator that waits for each request to finish
before sending the next one *cannot observe* queueing — it omits the
latencies it causes. Our harness must measure each operation's service time
individually (start/stop timestamps around the op) and record it in the
HDR-style histogram, rather than inferring latency from throughput.

## 6. Reporting

- Always report **p50 / p99 / p99.9 / max** in nanoseconds, plus throughput
  (msgs/sec) where meaningful. Never report only the mean.
- Include `perf stat` cache-miss and branch-miss data for microbenchmarks.
- Every published result carries a footer: CPU model, core count, OS/kernel,
  compiler + version + flags, governor, isolation level, and the exact
  command run.
- "What I tried that did not help" is a required section of every benchmark
  report — negative results are data.
