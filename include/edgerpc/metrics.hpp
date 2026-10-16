#pragma once

#include <vector>
#include <cstdint>

namespace edgerpc {

// POSIX CLOCK_MONOTONIC, shared kernel epoch for cross-process stage traces.
std::uint64_t monotonic_ns();
struct PipelineTrace {
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    double buffer_wait_us = 0;
    std::uint64_t prepare_begin_ns = 0;
    std::uint64_t prepare_end_ns = 0;
    std::uint64_t queued_ns = 0;
    std::uint64_t execute_begin_ns = 0;
    std::uint64_t execute_end_ns = 0;
};

struct LatencySummary {
    double p50_us;
    double p95_us;
    double p99_us;
};

// Nearest-rank percentiles of successful request latencies.
LatencySummary summarize_latencies(std::vector<double> samples);

} // namespace edgerpc
