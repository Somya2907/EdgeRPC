#pragma once

#include <vector>
#include "edgerpc/protocol.hpp"

namespace edgerpc {

class ExecutionBackend {
public:
    virtual ~ExecutionBackend() = default;
    // Must permit concurrent calls when the server is configured with >1 worker.
    virtual std::vector<float> execute(const std::vector<float>& input, Operation operation) = 0;
};

} // namespace edgerpc
