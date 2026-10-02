# PROJECT BRIEF: Kairos

## What we are building
A from-scratch, low-latency C++20 system with one deterministic core used in two modes:
1. SIMULATOR: replays recorded L2 market data into an order book, injects my strategy's orders with
   realistic order-entry latency and queue-position modelling, and produces fills I can trust.
2. ARENA: the same matching engine driven by simulated agents (noise traders, informed traders,
   other bots) so participants' market-making bots compete on a leaderboard.

Core thesis: most open-source backtesters assume fills at the price you see. We model latency and
queue position and VALIDATE the model against real fills. Validation is the differentiator.

## Why this exists (so you can judge trade-offs)
- I am learning quantitative finance, market microstructure, and low-latency C++ by building this.
- The goal is a public, credible project that demonstrates engineering depth and research rigor to
  high-frequency trading firms. Every claim must therefore be measurable and reproducible.
- I will publish write-ups at the end of each phase.

## About me
- Strong in backend / distributed systems (Java, Kafka, Flink). Still building depth in
  low-latency C++ and market microstructure. Explain C++ and finance concepts when you introduce them.
- I want to UNDERSTAND every line. I will be asked about this code in interviews.

## Non-negotiable engineering rules
- Language: C++20. Build: CMake (+ presets). Compilers: clang and gcc must both build clean with
  -Wall -Wextra -Wpedantic -Werror.
- Hot path (matching, book updates, event loop, strategy callbacks): NO heap allocation, NO locks,
  NO exceptions, NO virtual calls, NO std::map/std::unordered_map, NO iostream, NO floating point
  for prices or quantities. Use fixed-point integers (price in ticks, qty in lots).
- Determinism: same input + same seed => bit-identical output. No wall-clock reads or
  unseeded randomness inside the core. Time is an explicit input (nanosecond uint64).
- Single-threaded core. Concurrency only at the edges (recorder, I/O), via clearly defined queues.
- Data-oriented design: think about cache lines, memory layout, branch predictability.
- Tests: unit tests, a differential test against a slow naive reference implementation, and
  randomized/property tests. Sanitizers (ASan, UBSan; TSan where threads exist) must be clean.
- Benchmarks: Google Benchmark plus custom latency histograms (HDR-style). Pinned core, warmed up,
  report p50/p99/p99.9/max, never only averages. Avoid coordinated omission. Document methodology.
- NEVER state a performance number you did not measure. If you cannot run it, say so and give me
  the exact command to run.

## How I want you to work
1. For any non-trivial task: first propose a short plan and the key design decision(s) with
   alternatives and trade-offs. Wait for my OK before writing large amounts of code.
2. Implement in small, reviewable steps. Each step compiles and has tests.
3. After each step, give a short explanation of WHY it is built this way and ask me ONE question
   that checks my understanding of the most important idea.
4. Push back if my design is flawed or if a requirement is ambiguous. Do not guess silently:
   list assumptions explicitly.
5. Ask before adding any third-party dependency. Prefer the standard library.
6. Record important decisions as short ADRs in docs/adr/NNNN-title.md.
7. Keep a running docs/LEARNING.md: concepts I should be able to explain, with 2-3 line summaries.

## Definition of done (for every change)
Builds clean on clang+gcc, tests pass, sanitizers clean, benchmarks run, docs updated, no TODOs left
silently (list them).

## Safety
Never write code that places real orders with real money unless I explicitly request it, and then
only with: API keys that have no withdrawal permission, a hard max order size, a hard max position,
a kill switch, and a testnet-first run. Default is read-only market data.
