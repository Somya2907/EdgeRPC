#pragma once

#include "edgerpc/executor.hpp"
#include <chrono>

namespace edgerpc {

class MockExecutor final : public ExecutionBackend {
public:
    explicit MockExecutor(std::chrono::microseconds delay = std::chrono::microseconds{0});
    std::vector<float> execute(const std::vector<float>& input, Operation operation) override;
private:
    std::chrono::microseconds delay_;
};

} // namespace edgerpc
