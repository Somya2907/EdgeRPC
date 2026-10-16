#pragma once

#include "edgerpc/protocol.hpp"

#include <chrono>
#include <string>

namespace edgerpc {

using Timeout = std::chrono::milliseconds;
constexpr Timeout kDefaultTimeout{5000};

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd();
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;
    int get() const noexcept { return fd_; }
    void reset() noexcept;
private:
    int fd_;
};

UniqueFd connect_unix(const std::string& path, Timeout timeout = kDefaultTimeout);
// EOF is normal only before the first byte; partial EOF always throws.
bool read_exact(int fd, void* data, std::size_t bytes, Timeout timeout = kDefaultTimeout);
void write_all(int fd, const void* data, std::size_t bytes, Timeout timeout = kDefaultTimeout);
bool receive_frame(int fd, Frame& frame, Timeout timeout = kDefaultTimeout);
void send_frame(int fd, const Frame& frame, Timeout timeout = kDefaultTimeout);

class UnixListener {
public:
    explicit UnixListener(const std::string& path);
    ~UnixListener();
    UnixListener(const UnixListener&) = delete;
    UnixListener& operator=(const UnixListener&) = delete;
    int get() const noexcept { return fd_.get(); }
private:
    UniqueFd fd_;
    std::string path_;
    std::uint64_t device_ = 0;
    std::uint64_t inode_ = 0;
};

} // namespace edgerpc
