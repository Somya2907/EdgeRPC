#include "edgerpc/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace edgerpc {

LatencySummary summarize_latencies(std::vector<double> samples) {
    if (samples.empty()) throw std::invalid_argument("cannot summarize zero requests");
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](double q) {
        return samples[static_cast<std::size_t>(std::ceil(q * samples.size())) - 1];
    };
    return {percentile(0.50), percentile(0.95), percentile(0.99)};
}

} // namespace edgerpc
