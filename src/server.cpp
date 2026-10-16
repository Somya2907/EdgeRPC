#include "edgerpc/server.hpp"

#include <cerrno>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <system_error>

namespace edgerpc {
namespace {

void send_error(int fd, const Header& request, Status status, const std::string& message, Timeout timeout) {
    Frame response;
    response.header = request;
    response.header.type = MessageType::Error;
    response.header.status = status;
    response.payload.assign(message.begin(), message.end());
    response.header.payload_size = static_cast<std::uint32_t>(response.payload.size());
    send_frame(fd, response, timeout);
}

void serve_connection(int fd, ExecutionBackend& backend, Timeout timeout,
                      const volatile std::sig_atomic_t& stop) {
    while (!stop) {
        Frame request;
        if (!receive_frame(fd, request, timeout)) return;
        const auto& header = request.header;
        if (header.type != MessageType::Request || header.status != Status::Ok ||
            header.request_id == 0 || header.flags > static_cast<std::uint32_t>(Operation::Compute) ||
            request.payload.size() % 4 != 0) {
            send_error(fd, header, Status::InvalidRequest, "invalid request metadata or tensor size", timeout);
            continue;
        }
        Frame response;
        response.header = header;
        response.header.type = MessageType::Response;
        try {
            const auto input = decode_tensor(request.payload);
            const auto output = backend.execute(input, static_cast<Operation>(header.flags));
            if (output.size() != input.size()) throw std::runtime_error("backend output size mismatch");
            response.payload = encode_tensor(output);
        } catch (const std::exception&) {
            send_error(fd, header, Status::InternalError, "execution backend failed", timeout);
            continue;
        }
        send_frame(fd, response, timeout);
    }
}

} // namespace

void run_server(const ServerOptions& options, ExecutionBackend& backend,
                const volatile std::sig_atomic_t& stop) {
    if (options.timeout.count() <= 0) throw std::invalid_argument("timeout must be positive");
    UnixListener listener(options.socket_path);
    std::cout << "READY socket=" << options.socket_path << " workers=1 simulator=true" << std::endl;
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
        try { serve_connection(client.get(), backend, options.timeout, stop); }
        catch (const std::exception& error) {
            // An invalid or disconnected peer cannot terminate the listening service.
            if (!stop) std::cerr << "connection closed: " << error.what() << '\n';
        }
    }
}

} // namespace edgerpc
