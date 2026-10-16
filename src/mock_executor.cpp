#include "edgerpc/mock_executor.hpp"

#include <thread>

namespace edgerpc {

MockExecutor::MockExecutor(std::chrono::microseconds delay) : delay_(delay) {
    if (delay.count() < 0) throw std::invalid_argument("delay must be nonnegative");
}

std::vector<float> MockExecutor::execute(const std::vector<float>& input, Operation operation) {
    if (operation != Operation::Compute && operation != Operation::Noop)
        throw ProtocolError("unsupported operation");
    if (delay_.count() != 0) std::this_thread::sleep_for(delay_);
    auto output = input;
    if (operation == Operation::Compute) {
        for (auto& value : output) value = value * 2.0f + 1.0f;
    }
    return output;
}

} // namespace edgerpc
