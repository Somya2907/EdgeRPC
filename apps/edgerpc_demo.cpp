#include "cli.hpp"
#include "edgerpc/client.hpp"

#include <iostream>

int main(int argc, char** argv) {
    try {
        edgerpc::cli::Arguments args(argc, argv,
            {"--socket", "--mode", "--transport", "--buffer-slots", "--prepare-us", "--elements", "--requests", "--max-pending", "--workload", "--timeout-ms"},
            {"--verify", "--help"});
        if (args.has("--help")) {
            std::cout << "Usage: edgerpc_demo [--socket PATH] [--mode sync|async] [--elements 1024] "
                         "[--transport socket|shm|pool] [--buffer-slots 1|2|4] [--prepare-us 0] [--requests 1] [--max-pending 32] "
                         "[--workload compute|noop] [--verify] [--timeout-ms 5000]\n";
            return 0;
        }
        const auto mode = args.get("--mode", "sync");
        if (mode != "sync" && mode != "async") throw std::invalid_argument("--mode must be sync or async");
        const auto transport_name = args.get("--transport", "socket");
        if (transport_name != "socket" && transport_name != "shm" && transport_name != "pool")
            throw std::invalid_argument("--transport must be socket, shm, or pool");
        const auto transport = transport_name == "pool" ? edgerpc::TensorTransport::SharedPool :
            transport_name == "shm" ? edgerpc::TensorTransport::SharedMemory : edgerpc::TensorTransport::Socket;
        const auto requests = args.number("--requests", 1, 1, 1024);
        const auto capacity = args.number("--max-pending", 32, 1, 1024);
        const auto input = edgerpc::cli::make_input(args.number("--elements", 1024, 0, edgerpc::kMaxElements));
        edgerpc::PoolOptions pool;
        if (transport == edgerpc::TensorTransport::SharedPool) {
            pool.slots = args.number("--buffer-slots", 2, 1, 4);
            pool.max_elements = input.size();
            pool.prepare_delay = std::chrono::microseconds{args.number("--prepare-us", 0, 0, 1000000)};
            if (pool.slots == 3) throw std::invalid_argument("--buffer-slots must be 1, 2, or 4");
        } else if (args.has("--buffer-slots") || args.has("--prepare-us"))
            throw std::invalid_argument("pool options require --transport pool");
        const auto operation = edgerpc::cli::operation(args);
        const auto timeout = edgerpc::Timeout{args.number("--timeout-ms", 5000, 1, 600000)};
        edgerpc::EdgeRpcClient client(args.get("--socket", "/tmp/edgerpc.sock"), timeout, capacity, pool);
        std::vector<std::future<edgerpc::Response>> pending;
        if (mode == "async") {
            for (std::uint64_t i = 0; i < requests; ++i) pending.push_back(client.infer_async(input, operation, transport));
        }
        for (std::uint64_t i = 0; i < requests; ++i) {
            const auto response = mode == "async" ? pending[i].get() : client.infer_sync(input, operation, transport);
            if (args.has("--verify")) edgerpc::cli::verify(input, response.output, operation);
            std::cout << "request_id=" << response.id << " elements=" << response.output.size()
                      << " status=OK verified=" << (args.has("--verify") ? "true" : "false");
            if (response.pipeline.slot) std::cout << " slot=" << response.pipeline.slot << " generation=" << response.pipeline.generation;
            std::cout << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "edgerpc_demo: " << error.what() << '\n';
        return 1;
    }
}
