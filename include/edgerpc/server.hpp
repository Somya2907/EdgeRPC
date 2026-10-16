#pragma once

#include "edgerpc/executor.hpp"
#include "edgerpc/transport.hpp"
#include <csignal>

namespace edgerpc {

struct ServerOptions {
    std::string socket_path = "/tmp/edgerpc.sock";
    Timeout timeout = kDefaultTimeout;
};

// Serial baseline: a single connection and execution at a time.
void run_server(const ServerOptions& options, ExecutionBackend& backend,
                const volatile std::sig_atomic_t& stop);

} // namespace edgerpc
