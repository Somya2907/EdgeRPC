#pragma once

#include "edgerpc/protocol.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace edgerpc::cli {

class Arguments {
public:
    Arguments(int argc, char** argv, const std::vector<std::string>& value_options,
              const std::vector<std::string>& switches = {}) {
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (values_.count(option)) throw std::invalid_argument("duplicate option: " + option);
            bool known = false;
            for (const auto& candidate : switches) {
                if (option == candidate) { values_[option] = "true"; known = true; break; }
            }
            if (known) continue;
            for (const auto& candidate : value_options) {
                if (option == candidate) {
                    if (++i >= argc || std::string(argv[i]).rfind("--", 0) == 0)
                        throw std::invalid_argument("missing value for " + option);
                    values_[option] = argv[i];
                    known = true;
                    break;
                }
            }
            if (!known) throw std::invalid_argument("unknown option: " + option);
        }
    }
    bool has(const std::string& key) const { return values_.count(key) != 0; }
    std::string get(const std::string& key, const std::string& fallback) const {
        const auto found = values_.find(key);
        return found == values_.end() ? fallback : found->second;
    }
    std::uint64_t number(const std::string& key, std::uint64_t fallback,
                         std::uint64_t minimum, std::uint64_t maximum) const {
        if (!has(key)) return fallback;
        const auto text = get(key, "");
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument(key + " must be an unsigned integer");
        const auto value = std::stoull(text);
        if (value < minimum || value > maximum) throw std::invalid_argument(key + " is out of range");
        return value;
    }
private:
    std::map<std::string, std::string> values_;
};

inline Operation operation(const Arguments& args) {
    const auto workload = args.get("--workload", "compute");
    if (workload == "compute") return Operation::Compute;
    if (workload == "noop") return Operation::Noop;
    throw std::invalid_argument("--workload must be compute or noop");
}

inline std::vector<float> make_input(std::size_t elements) {
    std::vector<float> input(elements);
    for (std::size_t i = 0; i < elements; ++i)
        input[i] = static_cast<float>(static_cast<int>(i % 257) - 128) / 8.0f;
    return input;
}

inline void verify(const std::vector<float>& input, const std::vector<float>& output, Operation op) {
    if (input.size() != output.size()) throw std::runtime_error("output size verification failed");
    for (std::size_t i = 0; i < input.size(); ++i) {
        const float expected = op == Operation::Compute ? input[i] * 2.0f + 1.0f : input[i];
        if (!std::isfinite(output[i]) || std::fabs(expected - output[i]) > 1e-6f)
            throw std::runtime_error("output verification failed at element " + std::to_string(i));
    }
}

} // namespace edgerpc::cli
