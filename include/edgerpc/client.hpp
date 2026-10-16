#pragma once

#include "edgerpc/transport.hpp"
#include <future>
#include <memory>

namespace edgerpc {

struct Response {
    std::uint64_t id;
    std::vector<float> output;
    Status status = Status::Ok;
    // Client-side decoding/validation completion, before promise notification.
    std::chrono::steady_clock::time_point completed_at{};
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
                           std::size_t max_pending = 32);
    ~EdgeRpcClient();
    EdgeRpcClient(const EdgeRpcClient&) = delete;
    EdgeRpcClient& operator=(const EdgeRpcClient&) = delete;
    Response infer_sync(const std::vector<float>& input, Operation operation = Operation::Compute);
    std::future<Response> infer_async(std::vector<float> input, Operation operation = Operation::Compute);
    // Reject new calls, fail pending calls, interrupt I/O, and join owned threads.
    void close();
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace edgerpc
