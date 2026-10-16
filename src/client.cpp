#include "edgerpc/client.hpp"
#include "edgerpc/shared_buffer.hpp"

#include <algorithm>
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
        TensorTransport transport;
        bool ready_for_reply = false;
        std::shared_ptr<SharedBuffer> shared;
        SharedDescriptor descriptor;
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
    std::size_t reply_count = 0;
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
            reply_count = 0;
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

    std::future<Response> submit(std::vector<float> input, Operation operation, TensorTransport transport) {
        if (input.size() > kMaxElements) throw ProtocolError("tensor exceeds 8 MiB limit");
        if (operation != Operation::Compute && operation != Operation::Noop)
            throw std::invalid_argument("unsupported operation");
        if (transport != TensorTransport::Socket && transport != TensorTransport::SharedMemory)
            throw std::invalid_argument("unsupported tensor transport");
        auto call = std::make_shared<Pending>();
        call->elements = input.size();
        call->operation = operation;
        call->transport = transport;
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

    void reject_preparation(const std::shared_ptr<Pending>& call, std::exception_ptr error) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopped) return;
        call->promise.set_exception(error);
        pending.erase(call->id);
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
                request.header.request_id = submission.pending->id;
                request.header.flags = static_cast<std::uint32_t>(submission.pending->operation);
                try {
                    std::shared_ptr<SharedBuffer> shared;
                    SharedDescriptor descriptor;
                    if (submission.pending->transport == TensorTransport::SharedMemory) {
                        const auto bytes = submission.input.size() * sizeof(float);
                        shared = std::make_shared<SharedBuffer>(SharedBuffer::create(std::max<std::size_t>(8, bytes * 2)));
                        shared->write_tensor(0, submission.input);
                        descriptor = {shared->name(), shared->size(), 0, bytes,
                                      static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(submission.input.size())};
                        request.header.type = MessageType::ShmRequest;
                        request.payload = encode_shared_descriptor(descriptor);
                    } else request.payload = encode_tensor(submission.input);
                    request.header.payload_size = static_cast<std::uint32_t>(request.payload.size());
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (stopped) return;
                        submission.pending->shared = std::move(shared);
                        submission.pending->descriptor = std::move(descriptor);
                        submission.pending->ready_for_reply = true;
                        ++reply_count;
                    }
                } catch (...) {
                    // Allocation/preparation failed before any frame bytes were sent.
                    reject_preparation(submission.pending, std::current_exception());
                    continue;
                }
                changed.notify_all();
                send_frame(socket.get(), request, timeout);
            }
        } catch (...) { fail(std::current_exception()); }
    }

    void receive_loop() {
        try {
            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [&] { return stopped || reply_count != 0; });
                    if (stopped) return;
                }
                Frame frame;
                if (!receive_frame(socket.get(), frame, timeout))
                    throw std::runtime_error("server disconnected before response");
                std::shared_ptr<Pending> call;
                std::shared_ptr<SharedBuffer> shared;
                SharedDescriptor descriptor;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopped) return;
                    const auto found = pending.find(frame.header.request_id);
                    if (found == pending.end()) throw ProtocolError("unknown or duplicate response request ID");
                    call = found->second;
                    if (!call->ready_for_reply) throw ProtocolError("response arrived before request preparation");
                    shared = call->shared;
                    descriptor = call->descriptor;
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
                    const auto expected = call->transport == TensorTransport::SharedMemory ?
                        MessageType::ShmResponse : MessageType::Response;
                    if (frame.header.type != expected || frame.header.status != Status::Ok)
                        throw ProtocolError("unexpected response type or status");
                    if (call->transport == TensorTransport::SharedMemory) {
                        if (!shared || !frame.payload.empty()) throw ProtocolError("invalid SHM completion");
                        response.output = shared->read_tensor(descriptor.output_offset, call->elements);
                    } else {
                        if (frame.payload.size() != call->elements * sizeof(float))
                            throw ProtocolError("response tensor size mismatch");
                        response.output = decode_tensor(frame.payload);
                    }
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
                    --reply_count;
                }
            }
        } catch (...) { fail(std::current_exception()); }
    }
};

EdgeRpcClient::EdgeRpcClient(const std::string& path, Timeout timeout, std::size_t max_pending)
    : state_(std::make_unique<State>(path, timeout, max_pending)) {}
EdgeRpcClient::~EdgeRpcClient() = default;
void EdgeRpcClient::close() { state_->close(); }
std::future<Response> EdgeRpcClient::infer_async(std::vector<float> input, Operation operation, TensorTransport transport) {
    return state_->submit(std::move(input), operation, transport);
}
Response EdgeRpcClient::infer_sync(const std::vector<float>& input, Operation operation, TensorTransport transport) {
    return infer_async(input, operation, transport).get();
}

} // namespace edgerpc
