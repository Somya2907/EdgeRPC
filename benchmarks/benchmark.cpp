#include "cli.hpp"
#include "edgerpc/client.hpp"
#include "edgerpc/metrics.hpp"

#include <chrono>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <unistd.h>

int main(int argc, char** argv) {
    try {
        edgerpc::cli::Arguments args(argc, argv,
            {"--socket", "--mode", "--size-kib", "--inflight", "--requests", "--warmup",
             "--csv", "--workers", "--workload", "--timeout-ms"}, {"--help"});
        if (args.has("--help")) {
            std::cout << "Usage: edgerpc_benchmark [--socket PATH] [--mode socket-sync|socket-async] "
                         "[--size-kib 64] [--inflight 1] [--workers 1] [--requests 1000] [--warmup 100] "
                         "[--workload compute|noop] [--csv FILE] [--timeout-ms 5000]\n";
            return 0;
        }
        const auto mode = args.get("--mode", "socket-sync");
        if (mode != "socket-sync" && mode != "socket-async")
            throw std::invalid_argument("--mode must be socket-sync or socket-async");
        const auto inflight = args.number("--inflight", 1, 1, mode == "socket-sync" ? 1 : 1024);
        const auto workers = args.number("--workers", 1, 1, 16);
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
        edgerpc::EdgeRpcClient client(args.get("--socket", "/tmp/edgerpc.sock"), timeout, inflight);
        using Clock = std::chrono::steady_clock;
        struct Sample { std::uint64_t id; double latency_us; };
        std::vector<Sample> samples;
        samples.reserve(requests);
        std::vector<double> latencies;
        latencies.reserve(requests);
        const auto run_id = std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) + "_" + std::to_string(::getpid());
        auto collect = [&](const edgerpc::Response& response, Clock::time_point start, bool measured) {
            const double elapsed = std::chrono::duration<double, std::micro>(response.completed_at - start).count();
            edgerpc::cli::verify(input, response.output, operation);
            if (measured) {
                samples.push_back({response.id, elapsed});
                latencies.push_back(elapsed);
            }
        };
        auto run = [&](std::uint64_t count, bool measured) {
            if (mode == "socket-sync") {
                for (std::uint64_t i = 0; i < count; ++i) {
                    const auto start = Clock::now();
                    collect(client.infer_sync(input, operation), start, measured);
                }
            } else {
                struct Flight { Clock::time_point start; std::future<edgerpc::Response> result; };
                std::deque<Flight> window;
                std::uint64_t submitted = 0;
                while (submitted < count || !window.empty()) {
                    while (submitted < count && window.size() < inflight) {
                        const auto start = Clock::now();
                        window.push_back({start, client.infer_async(input, operation)});
                        ++submitted;
                    }
                    auto flight = std::move(window.front());
                    window.pop_front();
                    collect(flight.result.get(), flight.start, measured);
                }
            }
        };
        run(warmup, false);
        const auto wall_start = Clock::now();
        run(requests, true);
        const double seconds = std::chrono::duration<double>(Clock::now() - wall_start).count();
        const auto summary = edgerpc::summarize_latencies(std::move(latencies));
        if (csv.is_open()) {
            csv << std::fixed << std::setprecision(3);
            for (const auto& sample : samples) {
                csv << run_id << ',' << mode << ',' << size_kib * 1024 << ',' << inflight << ',' << workers
                    << ",0," << sample.id
                    << ",0," << sample.latency_us << ",,,," << args.get("--workload", "compute") << '\n';
            }
            csv.flush();
            if (!csv) throw std::runtime_error("failed writing CSV output");
        }
        std::cout << std::fixed << std::setprecision(3)
                  << "completed=" << requests << " failures=0 warmup=" << warmup
                  << " mode=" << mode << " tensor_bytes=" << size_kib * 1024
                  << " inflight=" << inflight << " workers=" << workers
                  << " p50_us=" << summary.p50_us << " p95_us=" << summary.p95_us
                  << " p99_us=" << summary.p99_us << " throughput_rps=" << requests / seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "edgerpc_benchmark: run failed: " << error.what() << '\n';
        return 1;
    }
}
