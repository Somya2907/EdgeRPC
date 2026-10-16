#include "edgerpc/client.hpp"

#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>

namespace edgerpc {

struct EdgeRpcClient::State {
    struct Pending {
        std::uint64_t id;
        std::size_t elements;
        Operation operation;
        std::promise<Response> promise;
    };
    struct Submission {
        std::shared_ptr<Pending> pending;
        std::vector<float> input;
    };

    UniqueFd socket;
    Timeout timeout;
    const std::size_t capacity;
    std::mutex mutex;
    std::mutex close_mutex;
    std::condition_variable changed;
    bool stopped = false;
    std::exception_ptr failure;
    std::uint64_t next_id = 1;
    std::deque<Submission> queue;
    std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pending;
    std::thread sender;
    std::thread receiver;

    State(const std::string& path, Timeout io_timeout, std::size_t max_pending)
        : socket(connect_unix(path, io_timeout)), timeout(io_timeout), capacity(max_pending) {
        if (capacity == 0 || capacity > 1024) throw std::invalid_argument("max_pending must be 1..1024");
        try {
            sender = std::thread([this] { send_loop(); });
            receiver = std::thread([this] { receive_loop(); });
        } catch (...) {
            fail(std::current_exception());
            if (sender.joinable()) sender.join();
            throw;
        }
    }

    ~State() { close(); }

    void fail(std::exception_ptr error) {
        std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> abandoned;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped) return;
            stopped = true;
            failure = error;
            pending.swap(abandoned);
            queue.clear();
            // Descriptor remains valid until both I/O threads have joined.
            ::shutdown(socket.get(), SHUT_RDWR);
        }
        changed.notify_all();
        for (auto& item : abandoned) item.second->promise.set_exception(error);
    }

    void close() {
        std::lock_guard<std::mutex> lock(close_mutex);
        fail(std::make_exception_ptr(std::runtime_error("client closed")));
        if (sender.joinable()) sender.join();
        if (receiver.joinable()) receiver.join();
        socket.reset();
    }

    std::future<Response> submit(std::vector<float> input, Operation operation) {
        if (input.size() > kMaxElements) throw ProtocolError("tensor exceeds 8 MiB limit");
        if (operation != Operation::Compute && operation != Operation::Noop)
            throw std::invalid_argument("unsupported operation");
        auto call = std::make_shared<Pending>();
        call->elements = input.size();
        call->operation = operation;
        auto future = call->promise.get_future();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped) { call->promise.set_exception(failure); return future; }
            if (pending.size() >= capacity) {
                call->promise.set_exception(std::make_exception_ptr(
                    RpcError(Status::Overloaded, "client pending-request capacity reached")));
                return future;
            }
            if (next_id == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("request IDs exhausted; create a new client");
            call->id = next_id++;
            pending.emplace(call->id, call);
            try { queue.push_back({call, std::move(input)}); }
            catch (...) { pending.erase(call->id); throw; }
        }
        changed.notify_all();
        return future;
    }

    void send_loop() {
        try {
            for (;;) {
                Submission submission;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [&] { return stopped || !queue.empty(); });
                    if (stopped) return;
                    submission = std::move(queue.front());
                    queue.pop_front();
                }
                Frame request;
                request.payload = encode_tensor(submission.input);
                request.header.request_id = submission.pending->id;
                request.header.flags = static_cast<std::uint32_t>(submission.pending->operation);
                request.header.payload_size = static_cast<std::uint32_t>(request.payload.size());
                send_frame(socket.get(), request, timeout);
            }
        } catch (...) { fail(std::current_exception()); }
    }

    void receive_loop() {
        try {
            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [&] { return stopped || !pending.empty(); });
                    if (stopped) return;
                }
                Frame frame;
                if (!receive_frame(socket.get(), frame, timeout))
                    throw std::runtime_error("server disconnected before response");
                std::shared_ptr<Pending> call;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopped) return;
                    const auto found = pending.find(frame.header.request_id);
                    if (found == pending.end()) throw ProtocolError("unknown or duplicate response request ID");
                    call = found->second;
                }
                if (frame.header.flags != static_cast<std::uint32_t>(call->operation))
                    throw ProtocolError("response operation mismatch");
                std::exception_ptr remote_error;
                Response response{call->id, {}, frame.header.status};
                if (frame.header.type == MessageType::Error) {
                    if (frame.header.status != Status::InvalidRequest &&
                        frame.header.status != Status::InternalError && frame.header.status != Status::Overloaded)
                        throw ProtocolError("invalid error status");
                    remote_error = std::make_exception_ptr(RpcError(frame.header.status,
                        std::string(frame.payload.begin(), frame.payload.end())));
                } else {
                    if (frame.header.type != MessageType::Response || frame.header.status != Status::Ok)
                        throw ProtocolError("unexpected response type or status");
                    if (frame.payload.size() != call->elements * sizeof(float))
                        throw ProtocolError("response tensor size mismatch");
                    response.output = decode_tensor(frame.payload);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopped) return;
                    // Keep the capacity reserved until the future is actually ready.
                    if (remote_error) call->promise.set_exception(remote_error);
                    else {
                        response.completed_at = std::chrono::steady_clock::now();
                        call->promise.set_value(std::move(response));
                    }
                    pending.erase(call->id);
                }
            }
        } catch (...) { fail(std::current_exception()); }
    }
};

EdgeRpcClient::EdgeRpcClient(const std::string& path, Timeout timeout, std::size_t max_pending)
    : state_(std::make_unique<State>(path, timeout, max_pending)) {}
EdgeRpcClient::~EdgeRpcClient() = default;
void EdgeRpcClient::close() { state_->close(); }
std::future<Response> EdgeRpcClient::infer_async(std::vector<float> input, Operation operation) {
    return state_->submit(std::move(input), operation);
}
Response EdgeRpcClient::infer_sync(const std::vector<float>& input, Operation operation) {
    return infer_async(input, operation).get();
}

} // namespace edgerpc
