#include "edgerpc/client.hpp"

#include <limits>

namespace edgerpc {

EdgeRpcClient::EdgeRpcClient(const std::string& path, Timeout timeout)
    : socket_(connect_unix(path, timeout)), timeout_(timeout) {}

Response EdgeRpcClient::infer_sync(const std::vector<float>& input, Operation operation) {
    if (socket_.get() < 0) throw std::runtime_error("client is disconnected; create a new client");
    if (operation != Operation::Compute && operation != Operation::Noop)
        throw std::invalid_argument("unsupported operation");
    if (next_id_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("request IDs exhausted; create a new client");
    Frame request;
    request.payload = encode_tensor(input);
    request.header.request_id = next_id_++;
    request.header.payload_size = static_cast<std::uint32_t>(request.payload.size());
    request.header.flags = static_cast<std::uint32_t>(operation);
    try {
        send_frame(socket_.get(), request, timeout_);
        Frame response;
        if (!receive_frame(socket_.get(), response, timeout_))
            throw std::runtime_error("server disconnected before response");
        if (response.header.request_id != request.header.request_id)
            throw ProtocolError("response request ID mismatch");
        if (response.header.flags != request.header.flags)
            throw ProtocolError("response operation mismatch");
        if (response.header.type == MessageType::Error) {
            if (response.header.status == Status::Ok) throw ProtocolError("error frame has OK status");
            throw RpcError(response.header.status, std::string(response.payload.begin(), response.payload.end()));
        }
        if (response.header.type != MessageType::Response || response.header.status != Status::Ok)
            throw ProtocolError("unexpected response type or status");
        if (response.payload.size() != request.payload.size())
            throw ProtocolError("response tensor size mismatch");
        return {response.header.request_id, decode_tensor(response.payload), response.header.status};
    } catch (...) {
        // A late response after a timeout must never be mistaken for the next request.
        socket_.reset();
        throw;
    }
}

} // namespace edgerpc
