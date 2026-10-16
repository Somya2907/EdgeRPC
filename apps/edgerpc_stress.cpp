#include "cli.hpp"
#include "edgerpc/client.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

int main(int argc, char** argv) {
    try {
        edgerpc::cli::Arguments args(argc, argv,
            {"--socket", "--threads", "--requests", "--window", "--transport", "--buffer-slots",
             "--elements", "--close-after", "--timeout-ms"}, {"--help", "--allow-disconnect"});
        if (args.has("--help")) {
            std::cout << "Usage: edgerpc_stress --socket PATH [--threads 8] [--requests 128 per thread] "
                         "[--window 4 per thread] [--transport socket|shm|pool|mixed] [--buffer-slots 1|2|4] "
                         "[--elements 256] [--close-after 0] [--allow-disconnect] [--timeout-ms 1000]\n";
            return 0;
        }
        const auto threads = args.number("--threads", 8, 1, 32);
        const auto requests = args.number("--requests", 128, 1, 100000);
        const auto window = args.number("--window", 4, 1, 32);
        const auto elements = args.number("--elements", 256, 0, edgerpc::kMaxElements);
        const auto close_after = args.number("--close-after", 0, 0, threads * requests);
        const auto transport = args.get("--transport", "mixed");
        if (transport != "socket" && transport != "shm" && transport != "pool" && transport != "mixed")
            throw std::invalid_argument("unsupported stress transport");
        edgerpc::PoolOptions pool;
        if (transport == "pool" || transport == "mixed") {
            pool.slots = args.number("--buffer-slots", 2, 1, 4);
            if (pool.slots == 3) throw std::invalid_argument("pool slots must be 1, 2, or 4");
            pool.max_elements = elements;
        } else if (args.has("--buffer-slots")) throw std::invalid_argument("slots require pool or mixed transport");
        const bool expect_errors = args.has("--allow-disconnect") || close_after != 0;
        const auto timeout = edgerpc::Timeout{args.number("--timeout-ms", 1000, 1, 60000)};
        edgerpc::EdgeRpcClient client(args.get("--socket", "/tmp/edgerpc.sock"), timeout, threads * window, pool, true);
        std::atomic<std::uint64_t> submitted{0}, successful{0}, errors{0}, done{0};
        std::mutex mutex;
        std::condition_variable changed;
        std::set<std::uint64_t> ids;
        std::exception_ptr failure;
        bool start = false;
        const auto active_threshold = std::min<std::uint64_t>(16, threads * requests);
        std::vector<std::thread> callers;
        // Workers own each generated input through verification, exposing mismatched
        // IDs and early reuse that a repeated constant tensor would not reveal.
        auto call = [&](std::uint64_t thread) {
            try {
                { std::unique_lock<std::mutex> lock(mutex); changed.wait(lock, [&] { return start; }); }
                struct Flight {
                    std::vector<float> input;
                    edgerpc::Operation operation;
                    std::future<edgerpc::Response> result;
                };
                std::deque<Flight> flights;
                std::uint64_t issued = 0;
                while (issued < requests || !flights.empty()) {
                    while (issued < requests && flights.size() < window) {
                        const auto ordinal = thread * requests + issued;
                        std::vector<float> input(elements);
                        for (std::size_t i = 0; i < input.size(); ++i)
                            input[i] = static_cast<float>(static_cast<int>((ordinal * 31 + i) % 2048) - 1024) / 16;
                        if (!input.empty()) input.front() = static_cast<float>(ordinal);
                        const auto operation = ordinal % 5 == 0 ? edgerpc::Operation::Noop : edgerpc::Operation::Compute;
                        const auto kind = transport == "mixed" ? ordinal % 3 : transport == "socket" ? 0 : transport == "shm" ? 1 : 2;
                        const auto selected = kind == 0 ? edgerpc::TensorTransport::Socket :
                            kind == 1 ? edgerpc::TensorTransport::SharedMemory : edgerpc::TensorTransport::SharedPool;
                        auto result = client.infer_async(input, operation, selected);
                        flights.push_back({std::move(input), operation, std::move(result)});
                        ++issued;
                        { std::lock_guard<std::mutex> lock(mutex); ++submitted; }
                        changed.notify_all();
                    }
                    auto flight = std::move(flights.front());
                    flights.pop_front();
                    if (flight.result.wait_for(timeout + std::chrono::seconds{3}) != std::future_status::ready)
                        throw std::runtime_error("stress future failed to settle");
                    edgerpc::Response response;
                    try { response = flight.result.get(); }
                    catch (const edgerpc::RpcError&) { throw; } // Overload/protocol errors are never expected crash recovery.
                    catch (const edgerpc::ProtocolError& error) {
                        if (!expect_errors || std::string(error.what()).find("disconnected") == std::string::npos) throw;
                        ++errors;
                        continue;
                    }
                    catch (const std::runtime_error&) {
                        if (!expect_errors) throw;
                        ++errors;
                        continue;
                    }
                    edgerpc::cli::verify(flight.input, response.output, flight.operation);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (!ids.insert(response.id).second) throw std::runtime_error("duplicate stress completion ID");
                    }
                    ++successful;
                }
            } catch (...) {
                { std::lock_guard<std::mutex> lock(mutex); if (!failure) failure = std::current_exception(); }
                client.close(); // Interrupt other waiters before joining workers.
            }
            { std::lock_guard<std::mutex> lock(mutex); ++done; }
            changed.notify_all();
        };
        try { for (std::uint64_t thread = 0; thread < threads; ++thread) callers.emplace_back(call, thread); }
        catch (...) {
            { std::lock_guard<std::mutex> lock(mutex); start = true; }
            changed.notify_all();
            client.close();
            for (auto& caller : callers) caller.join();
            throw;
        }
        { std::lock_guard<std::mutex> lock(mutex); start = true; }
        changed.notify_all();
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return submitted >= active_threshold || done == threads; });
        }
        std::cout << "ACTIVE submissions=" << submitted << std::endl;
        if (close_after) {
            { std::unique_lock<std::mutex> lock(mutex); changed.wait(lock, [&] { return submitted >= close_after || done == threads; }); }
            client.close();
        }
        for (auto& caller : callers) caller.join();
        client.close();
        if (failure) std::rethrow_exception(failure);
        if (submitted != threads * requests || successful + errors != submitted || (expect_errors && errors == 0))
            throw std::runtime_error("stress accounting or expected disconnect mismatch");
        std::cout << "submitted=" << submitted << " successful=" << successful << " errors=" << errors
                  << " verified=true settled=true\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "edgerpc_stress: " << error.what() << '\n';
        return 1;
    }
}
