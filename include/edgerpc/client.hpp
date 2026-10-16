#pragma once

#include "edgerpc/transport.hpp"

namespace edgerpc {

struct Response {
    std::uint64_t id;
    std::vector<float> output;
    Status status = Status::Ok;
};

class RpcError : public std::runtime_error {
public:
    RpcError(Status status, const std::string& message) : std::runtime_error(message), status_(status) {}
    Status status() const noexcept { return status_; }
private:
    Status status_;
};

// One caller at a time per instance; one persistent connection, one in-flight request.
class EdgeRpcClient {
public:
    explicit EdgeRpcClient(const std::string& path, Timeout timeout = kDefaultTimeout);
    Response infer_sync(const std::vector<float>& input, Operation operation = Operation::Compute);
private:
    UniqueFd socket_;
    Timeout timeout_;
    std::uint64_t next_id_ = 1;
};

} // namespace edgerpc
