#include "edgerpc/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cerrno>
#include <ctime>
#include <system_error>

namespace edgerpc {

std::uint64_t monotonic_ns() {
    timespec time{};
    if (::clock_gettime(CLOCK_MONOTONIC, &time) != 0)
        throw std::system_error(errno, std::generic_category(), "clock_gettime");
    return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL + time.tv_nsec;
}

LatencySummary summarize_latencies(std::vector<double> samples) {
    if (samples.empty()) throw std::invalid_argument("cannot summarize zero requests");
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](double q) {
        return samples[static_cast<std::size_t>(std::ceil(q * samples.size())) - 1];
    };
    return {percentile(0.50), percentile(0.95), percentile(0.99)};
}

} // namespace edgerpc
