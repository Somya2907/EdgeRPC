#include "cli.hpp"
#include "edgerpc/mock_executor.hpp"
#include "edgerpc/server.hpp"

#include <csignal>
#include <iostream>
#include <sys/stat.h>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
extern "C" void request_stop(int) { stop_requested = 1; }
}

int main(int argc, char** argv) {
    try {
        edgerpc::cli::Arguments args(argc, argv,
            {"--socket", "--workers", "--queue-capacity", "--delay-us", "--timeout-ms"}, {"--help"});
        if (args.has("--help")) {
            std::cout << "Usage: edgerpc_server [--socket PATH] [--workers 1..16] [--queue-capacity 32] "
                         "[--delay-us 0..1000000] [--timeout-ms 5000]\n"
                         "Default: one execution worker. Extra workers model parallel CPU work, not an NPU.\n";
            return 0;
        }
        edgerpc::ServerOptions options;
        options.workers = args.number("--workers", 1, 1, 16);
        options.queue_capacity = args.number("--queue-capacity", 32, 1, 1024);
        options.socket_path = args.get("--socket", "/tmp/edgerpc.sock");
        options.timeout = edgerpc::Timeout{args.number("--timeout-ms", 5000, 1, 600000)};
        edgerpc::MockExecutor backend(std::chrono::microseconds{
            args.number("--delay-us", 0, 0, 1000000)});
        struct sigaction action{};
        action.sa_handler = request_stop;
        // Darwin exposes sigemptyset as a macro; a leading :: breaks expansion.
        sigemptyset(&action.sa_mask);
        if (::sigaction(SIGINT, &action, nullptr) < 0 || ::sigaction(SIGTERM, &action, nullptr) < 0)
            throw std::runtime_error("cannot install shutdown signal handlers");
        ::umask(0077);
        edgerpc::run_server(options, backend, stop_requested);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "edgerpc_server: " << error.what() << '\n';
        return 1;
    }
}
