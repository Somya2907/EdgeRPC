#include "cli.hpp"
#include "edgerpc/client.hpp"
#include "edgerpc/metrics.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <unistd.h>

int main(int argc, char** argv) {
    try {
        edgerpc::cli::Arguments args(argc, argv,
            {"--socket", "--mode", "--size-kib", "--inflight", "--requests", "--warmup",
             "--csv", "--workload", "--timeout-ms"}, {"--help"});
        if (args.has("--help")) {
            std::cout << "Usage: edgerpc_benchmark [--socket PATH] [--mode socket-sync] "
                         "[--size-kib 64] [--inflight 1] [--requests 1000] [--warmup 100] "
                         "[--workload compute|noop] [--csv FILE] [--timeout-ms 5000]\n";
            return 0;
        }
        if (args.get("--mode", "socket-sync") != "socket-sync")
            throw std::invalid_argument("only socket-sync mode is implemented");
        args.number("--inflight", 1, 1, 1);
        const auto size_kib = args.number("--size-kib", 64, 0, edgerpc::kMaxPayloadBytes / 1024);
        const auto requests = args.number("--requests", 1000, 1, 1000000);
        const auto warmup = args.number("--warmup", 100, 0, 1000000);
        const auto operation = edgerpc::cli::operation(args);
        const auto input = edgerpc::cli::make_input(size_kib * 1024 / sizeof(float));
        const auto timeout = edgerpc::Timeout{args.number("--timeout-ms", 5000, 1, 600000)};
        // Check the destination before doing a potentially long experiment.
        std::ofstream csv;
        if (args.has("--csv")) {
            csv.open(args.get("--csv", ""));
            if (!csv) throw std::runtime_error("cannot open CSV output");
            csv << "run_id,mode,tensor_bytes,inflight,workers,buffer_slots,request_id,status,latency_us,"
                   "queue_us,execute_us,buffer_wait_us,workload\n";
        }
        edgerpc::EdgeRpcClient client(args.get("--socket", "/tmp/edgerpc.sock"), timeout);
        for (std::uint64_t i = 0; i < warmup; ++i) {
            auto response = client.infer_sync(input, operation);
            edgerpc::cli::verify(input, response.output, operation);
        }
        using Clock = std::chrono::steady_clock;
        struct Sample { std::uint64_t id; double latency_us; };
        std::vector<Sample> samples;
        samples.reserve(requests);
        std::vector<double> latencies;
        latencies.reserve(requests);
        const auto run_id = std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) + "_" + std::to_string(::getpid());
        const auto wall_start = Clock::now();
        for (std::uint64_t i = 0; i < requests; ++i) {
            const auto start = Clock::now();
            const auto response = client.infer_sync(input, operation);
            const double elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            edgerpc::cli::verify(input, response.output, operation);
            samples.push_back({response.id, elapsed});
            latencies.push_back(elapsed);
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - wall_start).count();
        const auto summary = edgerpc::summarize_latencies(std::move(latencies));
        if (csv.is_open()) {
            csv << std::fixed << std::setprecision(3);
            for (const auto& sample : samples) {
                csv << run_id << ",socket-sync," << size_kib * 1024 << ",1,1,0," << sample.id
                    << ",0," << sample.latency_us << ",,,," << args.get("--workload", "compute") << '\n';
            }
            csv.flush();
            if (!csv) throw std::runtime_error("failed writing CSV output");
        }
        std::cout << std::fixed << std::setprecision(3)
                  << "completed=" << requests << " failures=0 warmup=" << warmup
                  << " tensor_bytes=" << size_kib * 1024 << " inflight=1 workers=1"
                  << " p50_us=" << summary.p50_us << " p95_us=" << summary.p95_us
                  << " p99_us=" << summary.p99_us << " throughput_rps=" << requests / seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "edgerpc_benchmark: run failed: " << error.what() << '\n';
        return 1;
    }
}
