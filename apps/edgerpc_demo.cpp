#include "cli.hpp"
#include "edgerpc/client.hpp"

#include <iostream>

int main(int argc, char** argv) {
    try {
        edgerpc::cli::Arguments args(argc, argv,
            {"--socket", "--mode", "--elements", "--workload", "--timeout-ms"}, {"--verify", "--help"});
        if (args.has("--help")) {
            std::cout << "Usage: edgerpc_demo [--socket PATH] [--mode sync] [--elements 1024] "
                         "[--workload compute|noop] [--verify] [--timeout-ms 5000]\n";
            return 0;
        }
        if (args.get("--mode", "sync") != "sync") throw std::invalid_argument("only sync mode is implemented");
        const auto input = edgerpc::cli::make_input(args.number("--elements", 1024, 0, edgerpc::kMaxElements));
        const auto operation = edgerpc::cli::operation(args);
        const auto timeout = edgerpc::Timeout{args.number("--timeout-ms", 5000, 1, 600000)};
        edgerpc::EdgeRpcClient client(args.get("--socket", "/tmp/edgerpc.sock"), timeout);
        const auto response = client.infer_sync(input, operation);
        if (args.has("--verify")) edgerpc::cli::verify(input, response.output, operation);
        std::cout << "request_id=" << response.id << " elements=" << response.output.size()
                  << " status=OK verified=" << (args.has("--verify") ? "true" : "false") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "edgerpc_demo: " << error.what() << '\n';
        return 1;
    }
}
