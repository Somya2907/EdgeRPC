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
             "--csv", "--workers", "--buffer-slots", "--prepare-us", "--workload", "--timeout-ms"}, {"--help"});
        if (args.has("--help")) {
            std::cout << "Usage: edgerpc_benchmark [--socket PATH] [--mode socket-sync|socket-async|shm-async|shm-double-buffer] "
                         "[--size-kib 64] [--inflight 1] [--workers 1] [--requests 1000] [--warmup 100] "
                         "[--buffer-slots 1|2|4] [--prepare-us 0] [--workload compute|noop] [--csv FILE] [--timeout-ms 5000]\n";
            return 0;
        }
        const auto mode = args.get("--mode", "socket-sync");
        const bool pooled = mode == "shm-double-buffer";
        if (mode != "socket-sync" && mode != "socket-async" && mode != "shm-async" && !pooled)
            throw std::invalid_argument("unsupported benchmark mode");
        const auto transport = pooled ? edgerpc::TensorTransport::SharedPool :
            mode == "shm-async" ? edgerpc::TensorTransport::SharedMemory : edgerpc::TensorTransport::Socket;
        const auto inflight = args.number("--inflight", pooled ? 2 : 1, 1, mode == "socket-sync" ? 1 : 1024);
        const auto workers = args.number("--workers", 1, 1, 16);
        const auto size_kib = args.number("--size-kib", 64, 0, edgerpc::kMaxPayloadBytes / 1024);
        const auto requests = args.number("--requests", 1000, 1, 1000000);
        const auto warmup = args.number("--warmup", 100, 0, 1000000);
        const auto operation = edgerpc::cli::operation(args);
        const auto input = edgerpc::cli::make_input(size_kib * 1024 / sizeof(float));
        edgerpc::PoolOptions pool;
        if (pooled) {
            pool.slots = args.number("--buffer-slots", 2, 1, 4);
            if (pool.slots == 3) throw std::invalid_argument("--buffer-slots must be 1, 2, or 4");
            pool.max_elements = input.size();
            pool.prepare_delay = std::chrono::microseconds{args.number("--prepare-us", 0, 0, 1000000)};
        } else if (args.has("--buffer-slots") || args.has("--prepare-us"))
            throw std::invalid_argument("pool options require shm-double-buffer mode");
        const auto timeout = edgerpc::Timeout{args.number("--timeout-ms", 5000, 1, 600000)};
        // Check the destination before doing a potentially long experiment.
        std::ofstream csv;
        if (args.has("--csv")) {
            csv.open(args.get("--csv", ""));
            if (!csv) throw std::runtime_error("cannot open CSV output");
            csv << "run_id,mode,tensor_bytes,inflight,workers,buffer_slots,request_id,status,latency_us,"
                   "queue_us,execute_us,buffer_wait_us,workload,slot,generation,prepare_begin_ns,prepare_end_ns,"
                   "queued_ns,execute_begin_ns,execute_end_ns\n";
        }
        const auto init_begin = std::chrono::steady_clock::now();
        edgerpc::EdgeRpcClient client(args.get("--socket", "/tmp/edgerpc.sock"), timeout, inflight, pool);
        const auto initialization_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - init_begin).count();
        using Clock = std::chrono::steady_clock;
        struct Sample { std::uint64_t id; double latency_us; edgerpc::PipelineTrace pipeline; };
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
                samples.push_back({response.id, elapsed, response.pipeline});
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
                        window.push_back({start, client.infer_async(input, operation, transport)});
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
                    << ',' << pool.slots << ',' << sample.id << ",0," << sample.latency_us << ',';
                const auto& trace = sample.pipeline;
                if (pooled) csv << (trace.execute_begin_ns - trace.queued_ns) / 1000.0 << ','
                                << (trace.execute_end_ns - trace.execute_begin_ns) / 1000.0 << ',' << trace.buffer_wait_us;
                else csv << ",,";
                csv << ',' << args.get("--workload", "compute") << ',';
                if (pooled) csv << trace.slot << ',' << trace.generation << ',' << trace.prepare_begin_ns << ','
                                << trace.prepare_end_ns << ',' << trace.queued_ns << ','
                                << trace.execute_begin_ns << ',' << trace.execute_end_ns;
                else csv << ",,,,,,";
                csv << '\n';
            }
            csv.flush();
            if (!csv) throw std::runtime_error("failed writing CSV output");
        }
        std::cout << std::fixed << std::setprecision(3)
                  << "completed=" << requests << " failures=0 warmup=" << warmup
                  << " mode=" << mode << " tensor_bytes=" << size_kib * 1024
                  << " inflight=" << inflight << " workers=" << workers
                  << " buffer_slots=" << pool.slots << " prepare_us=" << pool.prepare_delay.count()
                  << " initialization_us=" << initialization_us
                  << " p50_us=" << summary.p50_us << " p95_us=" << summary.p95_us
                  << " p99_us=" << summary.p99_us << " throughput_rps=" << requests / seconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "edgerpc_benchmark: run failed: " << error.what() << '\n';
        return 1;
    }
}
