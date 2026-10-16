#include "edgerpc/server.hpp"
#include "edgerpc/thread_pool.hpp"

#include <atomic>
#include <cerrno>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <system_error>

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

struct Connection {
    int fd;
    Timeout timeout;
    std::mutex send_mutex;
    std::atomic<bool> alive{true};
    std::atomic<std::size_t> outstanding{0};

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

void execute_request(const std::shared_ptr<Connection>& connection,
                     ExecutionBackend& backend, const Frame& request) {
    try {
        if (connection->alive) {
            Frame response;
            response.header = request.header;
            response.header.type = MessageType::Response;
            try {
                const auto input = decode_tensor(request.payload);
                const auto output = backend.execute(input, static_cast<Operation>(request.header.flags));
                if (output.size() != input.size()) throw std::runtime_error("backend output size mismatch");
                response.payload = encode_tensor(output);
            } catch (...) {
                response = error_frame(request.header, Status::InternalError, "execution backend failed");
            }
            connection->send(response);
        }
    } catch (...) { connection->close(); }
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
            pollfd descriptor{fd, POLLIN, 0};
            const int result = ::poll(&descriptor, 1, 100);
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
            if (header.type != MessageType::Request || header.status != Status::Ok ||
                header.request_id == 0 || header.flags > static_cast<std::uint32_t>(Operation::Compute) ||
                request.payload.size() % 4 != 0) {
                connection->send(error_frame(header, Status::InvalidRequest, "invalid request metadata or tensor size"));
                continue;
            }
            ++connection->outstanding;
            if (!pool.try_submit([connection, &backend, request = std::move(request)] {
                execute_request(connection, backend, request);
            })) {
                --connection->outstanding;
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
