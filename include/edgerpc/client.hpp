#pragma once

#include "edgerpc/transport.hpp"
#include "edgerpc/metrics.hpp"
#include <future>
#include <memory>

namespace edgerpc {

struct Response {
    std::uint64_t id;
    std::vector<float> output;
    Status status = Status::Ok;
    // Client-side decoding/validation completion, before promise notification.
    std::chrono::steady_clock::time_point completed_at{};
    PipelineTrace pipeline{};
};

struct PoolOptions {
    std::size_t slots = 0; // Disabled by default; enable 1, 2, or 4 fixed-capacity slots.
    std::size_t max_elements = 0;
    std::chrono::microseconds prepare_delay{0}; // Explicit simulated preparation for experiments.
};

class RpcError : public std::runtime_error {
public:
    RpcError(Status status, const std::string& message) : std::runtime_error(message), status_(status) {}
    Status status() const noexcept { return status_; }
private:
    Status status_;
};

// Thread-safe submissions on one connection. Capacity includes queued and sent calls.
class EdgeRpcClient {
public:
    explicit EdgeRpcClient(const std::string& path, Timeout timeout = kDefaultTimeout,
                           std::size_t max_pending = 32, PoolOptions pool = {});
    ~EdgeRpcClient();
    EdgeRpcClient(const EdgeRpcClient&) = delete;
    EdgeRpcClient& operator=(const EdgeRpcClient&) = delete;
    Response infer_sync(const std::vector<float>& input, Operation operation = Operation::Compute,
                        TensorTransport transport = TensorTransport::Socket);
    std::future<Response> infer_async(std::vector<float> input, Operation operation = Operation::Compute,
                                     TensorTransport transport = TensorTransport::Socket);
    // Reject new calls, fail pending calls, interrupt I/O, and join owned threads.
    void close();
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace edgerpc
