#pragma once

// Custom latency harness: skeleton.
//
// Google Benchmark is throughput-oriented (it reports mean time/op). For the
// engine we need per-operation latency DISTRIBUTIONS: p50/p99/p99.9/max.
// Phase 1 implements an HDR-style histogram here: fixed-bucket, records raw
// nanosecond samples with no allocation, and is careful to avoid coordinated
// omission (see docs/benchmarks/METHODOLOGY.md).
//
// Interface sketch (implemented in Phase 1):
//
//   class LatencyHistogram {
//   public:
//       void record(std::uint64_t ns) noexcept;  // one measured operation
//       // ... percentile queries, merge, reset ...
//   };
