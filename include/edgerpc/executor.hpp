#pragma once

#include <vector>
#include "edgerpc/protocol.hpp"

namespace edgerpc {

class ExecutionBackend {
public:
    virtual ~ExecutionBackend() = default;
    virtual std::vector<float> execute(const std::vector<float>& input, Operation operation) = 0;
};

} // namespace edgerpc
