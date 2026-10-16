#pragma once

#include "edgerpc/executor.hpp"
#include "edgerpc/transport.hpp"
#include <csignal>

namespace edgerpc {

struct ServerOptions {
    std::string socket_path = "/tmp/edgerpc.sock";
    Timeout timeout = kDefaultTimeout;
    std::size_t workers = 1;
    std::size_t queue_capacity = 32;
};

// One active connection; bounded queued execution within that connection.
void run_server(const ServerOptions& options, ExecutionBackend& backend,
                const volatile std::sig_atomic_t& stop);

} // namespace edgerpc
