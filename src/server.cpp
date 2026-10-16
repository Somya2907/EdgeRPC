#include "edgerpc/server.hpp"
#include "edgerpc/thread_pool.hpp"
#include "edgerpc/shared_buffer.hpp"
#include "edgerpc/metrics.hpp"

#include <atomic>
#include <cerrno>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <system_error>
#include <unordered_map>

namespace edgerpc {
namespace {

Frame error_frame(const Header& request, Status status, const std::string& message) {
    Frame response;
    response.header = request;
    response.header.type = MessageType::Error;
    response.header.status = status;
    response.payload.assign(message.begin(), message.end());
    response.header.payload_size = static_cast<std::uint32_t>(response.payload.size());
    return response;
}

struct RegisteredSlot {
    std::shared_ptr<SharedBuffer> buffer;
    SharedDescriptor descriptor;
    bool busy = false;
    std::uint64_t generation = 0;
};

struct Connection {
    int fd;
    Timeout timeout;
    std::mutex send_mutex;
    std::atomic<bool> alive{true};
    std::atomic<std::size_t> outstanding{0};
    std::mutex slots_mutex;
    std::unordered_map<std::uint32_t, std::shared_ptr<RegisteredSlot>> slots;

    Connection(int descriptor, Timeout io_timeout) : fd(descriptor), timeout(io_timeout) {}
    void close() {
        if (alive.exchange(false)) ::shutdown(fd, SHUT_RDWR);
    }
    void send(const Frame& response) {
        // Workers and the reader (validation/overload errors) share one framed writer.
        std::lock_guard<std::mutex> lock(send_mutex);
        if (alive) send_frame(fd, response, timeout);
    }
};

void release_slot(const std::shared_ptr<Connection>& connection, const std::shared_ptr<RegisteredSlot>& slot) {
    if (slot) {
        std::lock_guard<std::mutex> lock(connection->slots_mutex);
        slot->busy = false;
    }
}

void register_buffer(const std::shared_ptr<Connection>& connection, const Frame& request) {
    if (request.header.status != Status::Ok || request.header.flags != 0 || request.header.request_id == 0) {
        connection->send(error_frame(request.header, Status::InvalidRequest, "invalid registration header"));
        return;
    }
    Frame ack;
    try {
        const auto registration = decode_registration(request.payload);
        {
            std::lock_guard<std::mutex> lock(connection->slots_mutex);
            if (connection->slots.count(registration.slot)) throw ProtocolError("slot already registered");
        }
        auto shared = std::make_shared<SharedBuffer>(SharedBuffer::open(
            registration.descriptor.name, registration.descriptor.region_bytes));
        shared->unlink_name();
        auto slot = std::make_shared<RegisteredSlot>();
        slot->buffer = std::move(shared);
        slot->descriptor = registration.descriptor;
        {
            std::lock_guard<std::mutex> lock(connection->slots_mutex);
            connection->slots.emplace(registration.slot, std::move(slot));
        }
        ack.header = request.header;
        ack.header.type = MessageType::RegisterAck;
        ack.header.payload_size = 4;
        ack.payload = encode_slot_id(registration.slot);
    } catch (const std::exception&) {
        ack = error_frame(request.header, Status::InvalidRequest, "invalid or unavailable buffer registration");
    }
    connection->send(ack);
}

void execute_request(const std::shared_ptr<Connection>& connection,
                     ExecutionBackend& backend, const Frame& request,
                     const std::shared_ptr<SharedBuffer>& shared, const SharedDescriptor& descriptor,
                     const std::shared_ptr<RegisteredSlot>& registered, const SlotReference& reference,
                     std::uint64_t queued_ns) {
    bool released = false;
    try {
        if (connection->alive) {
            Frame response;
            response.header = request.header;
            response.header.type = MessageType::Response;
            try {
                const bool timed = request.header.flags & kTraceFlag;
                const auto execute_begin = (registered || timed) ? monotonic_ns() : 0;
                const auto input = shared ? shared->read_tensor(descriptor.input_offset, descriptor.elements)
                                          : decode_tensor(request.payload);
                const auto output = backend.execute(input, static_cast<Operation>(request.header.flags & ~kTraceFlag));
                if (output.size() != input.size()) throw std::runtime_error("backend output size mismatch");
                if (registered) {
                    shared->write_tensor(descriptor.output_offset, output);
                    const auto execute_end = monotonic_ns();
                    response.header.type = MessageType::PooledResponse;
                    response.payload = encode_pool_completion({reference, queued_ns, execute_begin, execute_end});
                    response.header.payload_size = static_cast<std::uint32_t>(response.payload.size());
                } else if (shared) {
                    shared->write_tensor(descriptor.output_offset, output);
                    response.header.type = MessageType::ShmResponse;
                    response.header.payload_size = 0;
                } else response.payload = encode_tensor(output);
                if (timed && !registered) {
                    const auto trace = encode_execution_trace({queued_ns, execute_begin, monotonic_ns()});
                    response.payload.insert(response.payload.begin(), trace.begin(), trace.end());
                    response.header.type = shared ? MessageType::TimedShmResponse : MessageType::TimedResponse;
                    response.header.payload_size = static_cast<std::uint32_t>(response.payload.size());
                }
            } catch (...) {
                response = error_frame(request.header, Status::InternalError, "execution backend failed");
            }
            // No further mapped accesses after this point. The next request implicitly
            // acknowledges that the client copied the preceding output before reuse.
            release_slot(connection, registered);
            released = true;
            connection->send(response);
        }
    } catch (...) { connection->close(); }
    if (!released) release_slot(connection, registered);
    --connection->outstanding;
}

void serve_connection(int fd, ExecutionBackend& backend, const ServerOptions& options,
                      const volatile std::sig_atomic_t& stop) {
    auto connection = std::make_shared<Connection>(fd, options.timeout);
    ThreadPool pool(options.workers, options.queue_capacity);
    auto idle_since = std::chrono::steady_clock::now();
    try {
        while (!stop && connection->alive) {
            // Idle deadlines must not discard work that is still executing.
            const bool had_work = connection->outstanding != 0;
            if (had_work) idle_since = std::chrono::steady_clock::now();
            pollfd socket_event{fd, POLLIN, 0};
            const int result = ::poll(&socket_event, 1, 100);
            if (result < 0) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "poll connection");
            }
            if (result == 0) {
                if (had_work) {
                    // Work can finish during poll; start a fresh idle interval afterward.
                    idle_since = std::chrono::steady_clock::now();
                    continue;
                }
                if (std::chrono::steady_clock::now() - idle_since >= options.timeout &&
                    connection->outstanding == 0) throw std::runtime_error("idle connection timed out");
                continue;
            }
            Frame request;
            if (!receive_frame(fd, request, options.timeout)) break;
            idle_since = std::chrono::steady_clock::now();
            const auto header = request.header;
            if (header.type == MessageType::RegisterBuffer) {
                register_buffer(connection, request);
                continue;
            }
            if ((header.type != MessageType::Request && header.type != MessageType::ShmRequest &&
                 header.type != MessageType::PooledRequest) ||
                header.status != Status::Ok ||
                header.request_id == 0 || (header.flags & ~kTraceFlag) > static_cast<std::uint32_t>(Operation::Compute) ||
                (header.type == MessageType::Request && request.payload.size() % 4 != 0)) {
                connection->send(error_frame(header, Status::InvalidRequest, "invalid request metadata or tensor size"));
                continue;
            }
            std::shared_ptr<SharedBuffer> shared;
            SharedDescriptor descriptor;
            std::shared_ptr<RegisteredSlot> registered;
            SlotReference reference;
            if (header.type == MessageType::ShmRequest) {
                try {
                    descriptor = decode_shared_descriptor(request.payload);
                    shared = std::make_shared<SharedBuffer>(SharedBuffer::open(descriptor.name, descriptor.region_bytes));
                    // Admission transfers ownership: never need to reopen this per-request object.
                    shared->unlink_name();
                } catch (const std::exception&) {
                    connection->send(error_frame(header, Status::InvalidRequest, "invalid or unavailable shared-memory region"));
                    continue;
                }
            }
            if (header.type == MessageType::PooledRequest) {
                bool acquired = false;
                try {
                    reference = decode_slot_reference(request.payload);
                    {
                        std::lock_guard<std::mutex> lock(connection->slots_mutex);
                        const auto found = connection->slots.find(reference.slot);
                        if (found == connection->slots.end()) throw ProtocolError("unregistered slot");
                        registered = found->second;
                        if (registered->busy || reference.generation <= registered->generation ||
                            reference.elements > registered->descriptor.elements)
                            throw ProtocolError("busy slot, stale generation, or oversized tensor");
                        registered->busy = true;
                        registered->generation = reference.generation;
                        acquired = true;
                    }
                    shared = registered->buffer;
                    descriptor = registered->descriptor;
                    descriptor.elements = reference.elements;
                    descriptor.tensor_bytes = reference.elements * 4;
                } catch (const std::exception&) {
                    if (acquired) release_slot(connection, registered);
                    connection->send(error_frame(header, Status::InvalidRequest, "invalid or busy registered slot reference"));
                    continue;
                }
            }
            const auto queued_ns = (registered || (header.flags & kTraceFlag)) ? monotonic_ns() : 0;
            ++connection->outstanding;
            if (!pool.try_submit([connection, &backend, request = std::move(request), shared, descriptor, registered, reference, queued_ns] {
                execute_request(connection, backend, request, shared, descriptor, registered, reference, queued_ns);
            })) {
                --connection->outstanding;
                release_slot(connection, registered);
                connection->send(error_frame(header, Status::Overloaded, "server execution queue is full"));
            }
        }
    } catch (...) {
        connection->close();
        pool.shutdown();
        throw;
    }
    connection->close();
    // Cancel waiting tasks and join running work before the descriptor/backend can die.
    pool.shutdown();
}

} // namespace

void run_server(const ServerOptions& options, ExecutionBackend& backend,
                const volatile std::sig_atomic_t& stop) {
    if (options.timeout.count() <= 0) throw std::invalid_argument("timeout must be positive");
    if (options.workers == 0 || options.workers > 16 ||
        options.queue_capacity == 0 || options.queue_capacity > 1024)
        throw std::invalid_argument("workers must be 1..16 and queue capacity 1..1024");
    UnixListener listener(options.socket_path);
    std::cout << "READY socket=" << options.socket_path << " workers=" << options.workers
              << " queue_capacity=" << options.queue_capacity << " simulator=true" << std::endl;
    while (!stop) {
        pollfd descriptor{listener.get(), POLLIN, 0};
        const int result = ::poll(&descriptor, 1, 100);
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "poll listener");
        }
        if (result == 0 || stop) continue;
        UniqueFd client(::accept(listener.get(), nullptr, nullptr));
        if (client.get() < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw std::system_error(errno, std::generic_category(), "accept");
        }
        try { serve_connection(client.get(), backend, options, stop); }
        catch (const std::exception& error) {
            if (!stop) std::cerr << "connection closed: " << error.what() << '\n';
        }
    }
}

} // namespace edgerpc
