#pragma once

#include <vector>

namespace edgerpc {

struct LatencySummary {
    double p50_us;
    double p95_us;
    double p99_us;
};

// Nearest-rank percentiles of successful request latencies.
LatencySummary summarize_latencies(std::vector<double> samples);

} // namespace edgerpc
