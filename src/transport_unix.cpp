#include "edgerpc/transport.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace edgerpc {
namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void system_failure(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

Clock::time_point deadline_for(Timeout timeout) {
    if (timeout.count() <= 0 || timeout.count() > INT_MAX)
        throw std::invalid_argument("timeout must be between 1 and INT_MAX milliseconds");
    return Clock::now() + timeout;
}

void wait_io(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) throw std::runtime_error("socket I/O timed out");
        const auto ms = std::chrono::duration_cast<Timeout>(remaining).count();
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(std::min<long long>(ms + 1, INT_MAX)));
        if (result < 0) {
            if (errno == EINTR) continue;
            system_failure("poll");
        }
        if (result == 0) continue;
        if (descriptor.revents & POLLNVAL) throw std::runtime_error("invalid socket descriptor");
        // HUP/ERR are handled by the following recv/send/getsockopt.
        return;
    }
}

sockaddr_un address_for(const std::string& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
#ifdef __APPLE__
    address.sun_len = static_cast<unsigned char>(sizeof(address));
#endif
    if (path.empty() || path.size() >= sizeof(address.sun_path) || path.find('\0') != std::string::npos)
        throw std::invalid_argument("Unix socket path is empty, contains NUL, or is too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

UniqueFd new_socket() {
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (fd.get() < 0) system_failure("socket");
    const int flags = ::fcntl(fd.get(), F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC) < 0) system_failure("fcntl");
    return fd;
}

} // namespace

UniqueFd::~UniqueFd() { reset(); }
UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) { reset(); fd_ = std::exchange(other.fd_, -1); }
    return *this;
}
void UniqueFd::reset() noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

UniqueFd connect_unix(const std::string& path, Timeout timeout) {
    const auto deadline = deadline_for(timeout);
    const auto address = address_for(path);
    auto fd = new_socket();
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS) system_failure("connect");
        wait_io(fd.get(), POLLOUT, deadline);
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) < 0) system_failure("getsockopt");
        if (error != 0) throw std::system_error(error, std::generic_category(), "connect");
    }
    return fd;
}

bool read_exact(int fd, void* data, std::size_t bytes, Timeout timeout, IoCounters* counters) {
    const auto deadline = deadline_for(timeout);
    auto* out = static_cast<std::uint8_t*>(data);
    std::size_t received = 0;
    while (received < bytes) {
        if (Clock::now() >= deadline) throw std::runtime_error("socket read timed out");
        const auto count = ::recv(fd, out + received, bytes - received, MSG_DONTWAIT);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            if (counters) counters->received.fetch_add(count, std::memory_order_relaxed);
            continue;
        }
        if (count == 0) {
            if (received == 0) return false;
            throw ProtocolError("disconnected in the middle of a frame");
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) { wait_io(fd, POLLIN, deadline); continue; }
        system_failure("recv");
    }
    return true;
}

void write_all(int fd, const void* data, std::size_t bytes, Timeout timeout, IoCounters* counters) {
    const auto deadline = deadline_for(timeout);
    const auto* in = static_cast<const std::uint8_t*>(data);
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
        system_failure("setsockopt SO_NOSIGPIPE");
#else
#error "A per-socket or per-send SIGPIPE suppression mechanism is required"
#endif
    std::size_t sent = 0;
    while (sent < bytes) {
        if (Clock::now() >= deadline) throw std::runtime_error("socket write timed out");
        const auto count = ::send(fd, in + sent, bytes - sent, flags);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            if (counters) counters->sent.fetch_add(count, std::memory_order_relaxed);
            continue;
        }
        if (count == 0) throw std::runtime_error("socket write made no progress");
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) { wait_io(fd, POLLOUT, deadline); continue; }
        system_failure("send");
    }
}

bool receive_frame(int fd, Frame& frame, Timeout timeout, IoCounters* counters) {
    std::array<std::uint8_t, kHeaderBytes> bytes{};
    if (!read_exact(fd, bytes.data(), bytes.size(), timeout, counters)) return false;
    // Validate the advertised length before any payload allocation.
    const auto header = decode_header(bytes);
    std::vector<std::uint8_t> payload(header.payload_size);
    if (!payload.empty() && !read_exact(fd, payload.data(), payload.size(), timeout, counters))
        throw ProtocolError("disconnected before frame payload");
    frame = {header, std::move(payload)};
    return true;
}

void send_frame(int fd, const Frame& frame, Timeout timeout, IoCounters* counters) {
    if (frame.payload.size() != frame.header.payload_size)
        throw ProtocolError("header and payload lengths differ");
    const auto bytes = encode_header(frame.header);
    write_all(fd, bytes.data(), bytes.size(), timeout, counters);
    write_all(fd, frame.payload.data(), frame.payload.size(), timeout, counters);
}

UnixListener::UnixListener(const std::string& path) {
    const auto address = address_for(path);
    struct stat existing{};
    if (::lstat(path.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) throw std::runtime_error("refusing to remove a non-socket path");
        bool stale = false;
        try { auto probe = connect_unix(path, Timeout{200}); }
        catch (const std::system_error& error) {
            if (error.code().value() != ECONNREFUSED) throw;
            stale = true;
        }
        if (!stale) throw std::runtime_error("socket is already in use");
        if (::unlink(path.c_str()) < 0) system_failure("unlink stale socket");
    } else if (errno != ENOENT) system_failure("lstat socket");

    fd_ = new_socket();
    if (::bind(fd_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
        system_failure("bind");
    try {
        if (::chmod(path.c_str(), 0600) < 0) system_failure("chmod socket");
        if (::listen(fd_.get(), 16) < 0) system_failure("listen");
        struct stat created{};
        if (::lstat(path.c_str(), &created) < 0) system_failure("lstat bound socket");
        device_ = static_cast<std::uint64_t>(created.st_dev);
        inode_ = static_cast<std::uint64_t>(created.st_ino);
        path_ = path;
    } catch (...) {
        ::unlink(path.c_str());
        throw;
    }
}

UnixListener::~UnixListener() {
    struct stat current{};
    if (!path_.empty() && ::lstat(path_.c_str(), &current) == 0 &&
        static_cast<std::uint64_t>(current.st_dev) == device_ &&
        static_cast<std::uint64_t>(current.st_ino) == inode_) ::unlink(path_.c_str());
}

} // namespace edgerpc
