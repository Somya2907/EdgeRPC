#include "edgerpc/client.hpp"
#include "edgerpc/shared_buffer.hpp"
#include "edgerpc/buffer_pool.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
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
        bool send_complete = false;
        std::shared_ptr<SharedBuffer> shared;
        SharedDescriptor descriptor;
        std::optional<BufferLease> lease;
        PipelineTrace trace;
        std::promise<Response> promise;
    };
    struct Submission {
        std::shared_ptr<Pending> pending;
        std::vector<float> input;
    };

    UniqueFd socket;
    Timeout timeout;
    const std::size_t capacity;
    const PoolOptions pool_options;
    const bool collect_metrics;
    IoCounters counters;
    std::shared_ptr<BufferPool> pool;
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

    State(const std::string& path, Timeout io_timeout, std::size_t max_pending, PoolOptions options, bool metrics)
        : socket(connect_unix(path, io_timeout)), timeout(io_timeout), capacity(max_pending), pool_options(options), collect_metrics(metrics) {
        if (capacity == 0 || capacity > 1024) throw std::invalid_argument("max_pending must be 1..1024");
        if (options.prepare_delay.count() < 0 || options.prepare_delay.count() > 1000000 ||
            (options.slots == 0 && (options.max_elements != 0 || options.prepare_delay.count() != 0)))
            throw std::invalid_argument("invalid pool options or preparation delay");
        if (options.slots != 0) {
            pool = std::make_shared<BufferPool>(options.slots, options.max_elements);
            for (std::uint32_t slot = 1; slot <= options.slots; ++slot) {
                Frame registration;
                registration.header = {MessageType::RegisterBuffer, next_id++, 74, Status::Ok, 0};
                registration.payload = encode_registration({slot, pool->descriptor(slot)});
                send_frame(socket.get(), registration, timeout, &counters);
                Frame ack;
                if (!receive_frame(socket.get(), ack, timeout, &counters)) throw ProtocolError("disconnected during registration");
                if (ack.header.request_id != registration.header.request_id || ack.header.flags != 0)
                    throw ProtocolError("registration response mismatch");
                if (ack.header.type == MessageType::Error && ack.header.status != Status::Ok)
                    throw RpcError(ack.header.status, std::string(ack.payload.begin(), ack.payload.end()));
                if (ack.header.type != MessageType::RegisterAck || ack.header.status != Status::Ok ||
                    decode_slot_id(ack.payload) != slot) throw ProtocolError("invalid registration acknowledgement");
            }
        }
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
        std::shared_ptr<BufferPool> stopped_pool;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped) return;
            stopped = true;
            failure = error;
            stopped_pool = pool;
            pending.swap(abandoned);
            reply_count = 0;
            queue.clear();
            // Descriptor remains valid until both I/O threads have joined.
            ::shutdown(socket.get(), SHUT_RDWR);
        }
        if (stopped_pool) stopped_pool->stop();
        changed.notify_all();
        for (auto& item : abandoned) item.second->promise.set_exception(error);
    }

    void close() {
        std::lock_guard<std::mutex> lock(close_mutex);
        fail(std::make_exception_ptr(std::runtime_error("client closed")));
        if (sender.joinable()) sender.join();
        if (receiver.joinable()) receiver.join();
        socket.reset();
        pool.reset();
    }

    std::future<Response> submit(std::vector<float> input, Operation operation, TensorTransport transport) {
        if (input.size() > kMaxElements) throw ProtocolError("tensor exceeds 8 MiB limit");
        if (operation != Operation::Compute && operation != Operation::Noop)
            throw std::invalid_argument("unsupported operation");
        if (transport != TensorTransport::Socket && transport != TensorTransport::SharedMemory &&
            transport != TensorTransport::SharedPool)
            throw std::invalid_argument("unsupported tensor transport");
        if (transport == TensorTransport::SharedPool &&
            (pool_options.slots == 0 || input.size() > pool_options.max_elements))
            throw std::invalid_argument("shared pool is disabled or input exceeds registered capacity");
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
                request.header.flags = static_cast<std::uint32_t>(submission.pending->operation) | (collect_metrics ? kTraceFlag : 0);
                std::optional<BufferLease> lease;
                PipelineTrace trace;
                if (collect_metrics) trace.prepare_begin_ns = monotonic_ns();
                try {
                    std::shared_ptr<SharedBuffer> shared;
                    SharedDescriptor descriptor;
                    if (submission.pending->transport == TensorTransport::SharedPool) {
                        const auto wait_begin = monotonic_ns();
                        lease = pool->acquire();
                        trace.buffer_wait_us = (monotonic_ns() - wait_begin) / 1000.0;
                        trace.slot = lease->slot;
                        trace.generation = lease->generation;
                        trace.prepare_begin_ns = monotonic_ns();
                        if (pool_options.prepare_delay.count() != 0) {
                            std::unique_lock<std::mutex> lock(mutex);
                            changed.wait_for(lock, pool_options.prepare_delay, [&] { return stopped; });
                        }
                        lease->buffer->write_tensor(0, submission.input);
                        trace.prepare_end_ns = monotonic_ns();
                        request.header.type = MessageType::PooledRequest;
                        request.payload = encode_slot_reference({lease->slot,
                            static_cast<std::uint32_t>(submission.input.size()), lease->generation});
                    } else if (submission.pending->transport == TensorTransport::SharedMemory) {
                        const auto bytes = submission.input.size() * sizeof(float);
                        shared = std::make_shared<SharedBuffer>(SharedBuffer::create(std::max<std::size_t>(8, bytes * 2)));
                        shared->write_tensor(0, submission.input);
                        descriptor = {shared->name(), shared->size(), 0, bytes,
                                      static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(submission.input.size())};
                        request.header.type = MessageType::ShmRequest;
                        request.payload = encode_shared_descriptor(descriptor);
                    } else request.payload = encode_tensor(submission.input);
                    if (collect_metrics) trace.prepare_end_ns = monotonic_ns();
                    request.header.payload_size = static_cast<std::uint32_t>(request.payload.size());
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (stopped) {
                            if (lease) pool->release(*lease);
                            return;
                        }
                        submission.pending->shared = std::move(shared);
                        submission.pending->descriptor = std::move(descriptor);
                        submission.pending->lease = lease;
                        submission.pending->trace = trace;
                        if (lease) pool->mark_in_flight(*lease);
                        submission.pending->ready_for_reply = true;
                        ++reply_count;
                    }
                } catch (...) {
                    if (lease) pool->release(*lease);
                    // Allocation/preparation failed before any frame bytes were sent.
                    reject_preparation(submission.pending, std::current_exception());
                    continue;
                }
                changed.notify_all();
                send_frame(socket.get(), request, timeout, &counters);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    submission.pending->send_complete = true;
                }
                changed.notify_all();
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
                if (!receive_frame(socket.get(), frame, timeout, &counters))
                    throw std::runtime_error("server disconnected before response");
                std::shared_ptr<Pending> call;
                std::shared_ptr<SharedBuffer> shared;
                SharedDescriptor descriptor;
                std::optional<BufferLease> lease;
                PipelineTrace trace;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopped) return;
                    const auto found = pending.find(frame.header.request_id);
                    if (found == pending.end()) throw ProtocolError("unknown or duplicate response request ID");
                    call = found->second;
                    if (!call->ready_for_reply) throw ProtocolError("response arrived before request preparation");
                    shared = call->shared;
                    descriptor = call->descriptor;
                    lease = call->lease;
                    trace = call->trace;
                }
                if (frame.header.flags != (static_cast<std::uint32_t>(call->operation) | (collect_metrics ? kTraceFlag : 0)))
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
                    const auto expected = call->transport == TensorTransport::SharedPool ? MessageType::PooledResponse :
                        call->transport == TensorTransport::SharedMemory ?
                            (collect_metrics ? MessageType::TimedShmResponse : MessageType::ShmResponse) :
                            (collect_metrics ? MessageType::TimedResponse : MessageType::Response);
                    if (frame.header.type != expected || frame.header.status != Status::Ok)
                        throw ProtocolError("unexpected response type or status");
                    if (collect_metrics && call->transport != TensorTransport::SharedPool) {
                        if (frame.payload.size() < 24) throw ProtocolError("missing execution trace");
                        const auto execution = decode_execution_trace({frame.payload.begin(), frame.payload.begin() + 24});
                        trace.queued_ns = execution.queued_ns;
                        trace.execute_begin_ns = execution.execute_begin_ns;
                        trace.execute_end_ns = execution.execute_end_ns;
                        frame.payload.erase(frame.payload.begin(), frame.payload.begin() + 24);
                    }
                    if (call->transport == TensorTransport::SharedPool) {
                        if (!lease) throw ProtocolError("missing pool lease");
                        const auto completion = decode_pool_completion(frame.payload);
                        if (completion.reference.slot != lease->slot || completion.reference.generation != lease->generation ||
                            completion.reference.elements != call->elements)
                            throw ProtocolError("stale or mismatched pool completion");
                        trace.queued_ns = completion.queued_ns;
                        trace.execute_begin_ns = completion.execute_begin_ns;
                        trace.execute_end_ns = completion.execute_end_ns;
                        pool->mark_ready(*lease);
                        response.output = lease->buffer->read_tensor(lease->output_offset, call->elements);
                    } else if (call->transport == TensorTransport::SharedMemory) {
                        if (!shared || !frame.payload.empty()) throw ProtocolError("invalid SHM completion");
                        response.output = shared->read_tensor(descriptor.output_offset, call->elements);
                    } else {
                        if (frame.payload.size() != call->elements * sizeof(float))
                            throw ProtocolError("response tensor size mismatch");
                        response.output = decode_tensor(frame.payload);
                    }
                    response.pipeline = trace;
                }
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    // Quiescent snapshots after futures settle must include the full send.
                    changed.wait(lock, [&] { return stopped || call->send_complete; });
                    if (stopped) return;
                    if (lease) {
                        if (remote_error) pool->mark_ready(*lease);
                        pool->release(*lease); // Output is copied; preparing the next use is now safe.
                    }
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

EdgeRpcClient::EdgeRpcClient(const std::string& path, Timeout timeout, std::size_t max_pending, PoolOptions pool, bool collect_metrics)
    : state_(std::make_unique<State>(path, timeout, max_pending, pool, collect_metrics)) {}
EdgeRpcClient::~EdgeRpcClient() = default;
void EdgeRpcClient::close() { state_->close(); }
SocketTraffic EdgeRpcClient::traffic() const noexcept { return state_->counters.snapshot(); }
std::future<Response> EdgeRpcClient::infer_async(std::vector<float> input, Operation operation, TensorTransport transport) {
    return state_->submit(std::move(input), operation, transport);
}
Response EdgeRpcClient::infer_sync(const std::vector<float>& input, Operation operation, TensorTransport transport) {
    return infer_async(input, operation, transport).get();
}

} // namespace edgerpc
