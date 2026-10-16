#include "edgerpc/client.hpp"
#include "edgerpc/thread_pool.hpp"
#include "edgerpc/shared_buffer.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <poll.h>
#include <optional>
#include <set>
#include <system_error>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#ifdef __linux__
#include <csignal>
#include <sys/resource.h>
#endif

namespace {
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void must_fail(Function function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    check(failed, "expected failure");
}

template<class T> void ready(std::future<T>& future) {
    check(future.wait_for(3s) == std::future_status::ready, "future did not settle");
}

class Directory {
public:
    Directory() {
        std::array<char, 32> pattern{};
        const std::string prefix = "/tmp/erpc-async-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto result = ::mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~Directory() { ::rmdir(path.c_str()); }
    std::string path;
};

edgerpc::Frame request(int fd) {
    edgerpc::Frame result;
    check(edgerpc::receive_frame(fd, result, 2s), "missing request");
    return result;
}

void reply(int fd, const edgerpc::Frame& request) {
    auto response = request;
    response.header.type = edgerpc::MessageType::Response;
    std::optional<edgerpc::SharedBuffer> shared;
    edgerpc::SharedDescriptor descriptor;
    std::vector<float> values;
    if (request.header.type == edgerpc::MessageType::ShmRequest) {
        descriptor = edgerpc::decode_shared_descriptor(request.payload);
        shared.emplace(edgerpc::SharedBuffer::open(descriptor.name, descriptor.region_bytes));
        shared->unlink_name();
        values = shared->read_tensor(descriptor.input_offset, descriptor.elements);
    } else values = edgerpc::decode_tensor(request.payload);
    if (request.header.flags == 1) for (auto& value : values) value = value * 2.0f + 1.0f;
    if (shared) {
        shared->write_tensor(descriptor.output_offset, values);
        response.header.type = edgerpc::MessageType::ShmResponse;
        response.header.payload_size = 0;
        response.payload.clear();
    } else response.payload = edgerpc::encode_tensor(values);
    edgerpc::send_frame(fd, response, 2s);
}

template<class Peer, class Client> void with_peer(Peer serve, Client exercise) {
    Directory directory;
    const auto path = directory.path + "/rpc.sock";
    edgerpc::UnixListener listener(path);
    auto peer = std::async(std::launch::async, [&] {
        pollfd descriptor{listener.get(), POLLIN, 0};
        int result;
        do { result = ::poll(&descriptor, 1, 3000); } while (result < 0 && errno == EINTR);
        check(result > 0, "client did not connect");
        edgerpc::UniqueFd accepted(::accept(listener.get(), nullptr, nullptr));
        check(accepted.get() >= 0, "accept failed");
        serve(accepted.get());
    });
    std::exception_ptr client_error;
    try { exercise(path); } catch (...) { client_error = std::current_exception(); }
    peer.get();
    if (client_error) std::rethrow_exception(client_error);
}

void check_reversed_responses(edgerpc::TensorTransport transport, bool mixed = false) {
    with_peer([](int fd) {
        std::vector<edgerpc::Frame> frames;
        for (int i = 0; i < 32; ++i) frames.push_back(request(fd));
        for (auto i = frames.rbegin(); i != frames.rend(); ++i) reply(fd, *i);
    }, [transport, mixed](const std::string& path) {
        edgerpc::EdgeRpcClient client(path);
        std::vector<std::future<edgerpc::Response>> futures;
        for (int i = 0; i < 32; ++i) futures.push_back(client.infer_async({static_cast<float>(i)},
            edgerpc::Operation::Compute, mixed && i % 2 ? edgerpc::TensorTransport::Socket : transport));
        std::set<std::uint64_t> ids;
        std::vector<std::chrono::steady_clock::time_point> completions;
        for (int i = 0; i < 32; ++i) {
            ready(futures[i]);
            const auto result = futures[i].get();
            check(result.output == std::vector<float>{2.0f * i + 1.0f}, "reply matched the wrong request");
            ids.insert(result.id);
            completions.push_back(result.completed_at);
        }
        check(ids.size() == 32, "request IDs collided");
        check(completions.front() >= completions.back() &&
              completions.back() > std::chrono::steady_clock::time_point{},
              "completion timestamps must reflect decode order, not future observation order");
    });
}

void reversed_responses() { check_reversed_responses(edgerpc::TensorTransport::Socket); }
void reversed_shared_responses() { check_reversed_responses(edgerpc::TensorTransport::SharedMemory); }
void mixed_transport_responses() { check_reversed_responses(edgerpc::TensorTransport::SharedMemory, true); }

void shared_cleanup(bool explicit_close) {
    std::vector<edgerpc::SharedDescriptor> regions;
    std::promise<void> received;
    auto all_received = received.get_future();
    with_peer([&](int fd) {
        for (int i = 0; i < 8; ++i) regions.push_back(edgerpc::decode_shared_descriptor(request(fd).payload));
        received.set_value();
        if (explicit_close) {
            edgerpc::Frame frame;
            check(!edgerpc::receive_frame(fd, frame, 2s), "close must interrupt the connection");
        }
    }, [&](const std::string& path) {
        edgerpc::EdgeRpcClient client(path);
        std::vector<std::future<edgerpc::Response>> futures;
        for (int i = 0; i < 8; ++i) futures.push_back(client.infer_async({1}, edgerpc::Operation::Compute,
            edgerpc::TensorTransport::SharedMemory));
        ready(all_received);
        all_received.get();
        if (explicit_close) client.close();
        for (auto& future : futures) { ready(future); must_fail([&] { future.get(); }); }
    });
    for (const auto& region : regions)
        must_fail([&] { edgerpc::SharedBuffer::open(region.name, region.region_bytes); });
}

void shared_close_cleanup() { shared_cleanup(true); }
void shared_disconnect_cleanup() { shared_cleanup(false); }

#ifdef __linux__
void failed_shared_preparation_preserves_connection() {
    with_peer([](int fd) {
        reply(fd, request(fd)); // Only the later socket request should reach the peer.
        reply(fd, request(fd));
    }, [](const std::string& path) {
        edgerpc::EdgeRpcClient client(path, 100ms);
        {
            // Force an isolated allocation error without consuming shared-memory capacity.
            // Socket I/O is unaffected. Restore the process limit and signal handler on all exits.
            struct FileLimit {
                rlimit original{};
                struct sigaction original_signal{};
                FileLimit() {
                    check(::getrlimit(RLIMIT_FSIZE, &original) == 0, "getrlimit failed");
                    struct sigaction ignore{};
                    ignore.sa_handler = SIG_IGN;
                    ::sigemptyset(&ignore.sa_mask);
                    check(::sigaction(SIGXFSZ, &ignore, &original_signal) == 0, "sigaction failed");
                    rlimit limited = original;
                    limited.rlim_cur = 0;
                    if (::setrlimit(RLIMIT_FSIZE, &limited) != 0) {
                        ::sigaction(SIGXFSZ, &original_signal, nullptr);
                        throw std::runtime_error("setrlimit failed");
                    }
                }
                ~FileLimit() {
                    ::setrlimit(RLIMIT_FSIZE, &original);
                    ::sigaction(SIGXFSZ, &original_signal, nullptr);
                }
            } limit;
            auto failed = client.infer_async({1}, edgerpc::Operation::Compute, edgerpc::TensorTransport::SharedMemory);
            ready(failed);
            bool rejected = false;
            try { failed.get(); }
            catch (const std::system_error& error) { rejected = error.code().value() == EFBIG; }
            check(rejected, "allocation failure was not reported to its future");
        }
        std::this_thread::sleep_for(200ms); // An unsent request must not start the receive timeout.
        check(client.infer_sync({3}).output == std::vector<float>{7}, "preparation error closed socket transport");
        check(client.infer_sync({4}, edgerpc::Operation::Compute, edgerpc::TensorTransport::SharedMemory).output ==
              std::vector<float>{9}, "preparation failure did not reclaim pending capacity");
    });
}
#endif

void client_backpressure() {
    std::promise<void> release;
    auto gate = release.get_future();
    with_peer([&](int fd) {
        const auto first = request(fd);
        check(gate.wait_for(2s) == std::future_status::ready, "overload check stalled");
        reply(fd, first);
        reply(fd, request(fd));
    }, [&](const std::string& path) {
        edgerpc::EdgeRpcClient client(path, 2s, 1);
        auto first = client.infer_async({1});
        auto rejected = client.infer_async({2});
        check(rejected.wait_for(0s) == std::future_status::ready, "full queue must reject without waiting");
        bool overloaded = false;
        try { rejected.get(); }
        catch (const edgerpc::RpcError& error) { overloaded = error.status() == edgerpc::Status::Overloaded; }
        check(overloaded, "client capacity did not produce OVERLOADED");
        release.set_value();
        ready(first);
        check(first.get().output == std::vector<float>{3}, "accepted request failed");
        check(client.infer_sync({3}).output == std::vector<float>{7}, "capacity was not reclaimed");
    });
}

void disconnect_settles_all() {
    with_peer([](int fd) {
        for (int i = 0; i < 8; ++i) request(fd);
    }, [](const std::string& path) {
        edgerpc::EdgeRpcClient client(path);
        std::vector<std::future<edgerpc::Response>> futures;
        for (int i = 0; i < 8; ++i) futures.push_back(client.infer_async({1}));
        for (auto& future : futures) { ready(future); must_fail([&] { future.get(); }); }
        auto later = client.infer_async({2});
        ready(later);
        must_fail([&] { later.get(); });
    });
}

void close_settles_queued_work() {
    with_peer([](int fd) {
        edgerpc::Frame frame;
        try { while (edgerpc::receive_frame(fd, frame, 2s)) {} }
        catch (const edgerpc::ProtocolError&) {} // Closing may interrupt a partial outbound frame.
    }, [](const std::string& path) {
        edgerpc::EdgeRpcClient client(path);
        std::vector<std::future<edgerpc::Response>> futures;
        for (int i = 0; i < 16; ++i) futures.push_back(client.infer_async(std::vector<float>(256, 1)));
        client.close();
        client.close();
        for (auto& future : futures) { ready(future); must_fail([&] { future.get(); }); }
        auto later = client.infer_async({2});
        ready(later);
        must_fail([&] { later.get(); });
    });
}

void destruction_settles_futures() {
    with_peer([](int fd) {
        edgerpc::Frame frame;
        try { while (edgerpc::receive_frame(fd, frame, 2s)) {} }
        catch (const edgerpc::ProtocolError&) {}
    }, [](const std::string& path) {
        std::future<edgerpc::Response> future;
        {
            edgerpc::EdgeRpcClient client(path);
            future = client.infer_async({1});
        }
        ready(future);
        must_fail([&] { future.get(); });
    });
}

void timeout_settles_all() {
    with_peer([](int fd) {
        edgerpc::Frame frame;
        while (edgerpc::receive_frame(fd, frame, 2s)) {}
    }, [](const std::string& path) {
        edgerpc::EdgeRpcClient client(path, 100ms);
        std::vector<std::future<edgerpc::Response>> futures;
        for (int i = 0; i < 8; ++i) futures.push_back(client.infer_async({1}));
        for (auto& future : futures) { ready(future); must_fail([&] { future.get(); }); }
    });
}

void protocol_failure_settles_all() {
    with_peer([](int fd) {
        auto response = request(fd);
        request(fd);
        response.header.type = edgerpc::MessageType::Response;
        response.header.request_id = 999;
        edgerpc::send_frame(fd, response, 2s);
    }, [](const std::string& path) {
        edgerpc::EdgeRpcClient client(path);
        auto first = client.infer_async({1});
        auto second = client.infer_async({2});
        ready(first); ready(second);
        must_fail([&] { first.get(); });
        must_fail([&] { second.get(); });
    });
}

void remote_error_isolated() {
    with_peer([](int fd) {
        auto first = request(fd);
        auto second = request(fd);
        first.header.type = edgerpc::MessageType::Error;
        first.header.status = edgerpc::Status::Overloaded;
        first.payload = {'f','u','l','l'};
        first.header.payload_size = 4;
        edgerpc::send_frame(fd, first, 2s);
        reply(fd, second);
        reply(fd, request(fd));
    }, [](const std::string& path) {
        edgerpc::EdgeRpcClient client(path);
        auto first = client.infer_async({1});
        auto second = client.infer_async({2});
        ready(first);
        bool overloaded = false;
        try { first.get(); }
        catch (const edgerpc::RpcError& error) { overloaded = error.status() == edgerpc::Status::Overloaded; }
        check(overloaded, "remote overload was not propagated");
        ready(second);
        check(second.get().output == std::vector<float>{5}, "remote error poisoned another request");
        check(client.infer_sync({3}).output == std::vector<float>{7}, "remote error closed a healthy connection");
    });
}

void concurrent_callers() {
    with_peer([](int fd) {
        std::vector<edgerpc::Frame> frames;
        for (int i = 0; i < 64; ++i) frames.push_back(request(fd));
        for (auto i = frames.rbegin(); i != frames.rend(); ++i) reply(fd, *i);
    }, [](const std::string& path) {
        edgerpc::EdgeRpcClient client(path, 2s, 128);
        std::vector<std::future<std::vector<std::uint64_t>>> callers;
        for (int thread = 0; thread < 8; ++thread) {
            callers.push_back(std::async(std::launch::async, [&, thread] {
                std::vector<std::future<edgerpc::Response>> futures;
                for (int i = 0; i < 8; ++i) futures.push_back(client.infer_async({static_cast<float>(thread * 8 + i)}));
                std::vector<std::uint64_t> ids;
                for (int i = 0; i < 8; ++i) {
                    ready(futures[i]);
                    const auto result = futures[i].get();
                    check(result.output == std::vector<float>{static_cast<float>(2 * (thread * 8 + i) + 1)},
                          "concurrent request mismatch");
                    ids.push_back(result.id);
                }
                return ids;
            }));
        }
        std::set<std::uint64_t> ids;
        for (auto& caller : callers) {
            ready(caller);
            for (auto id : caller.get()) ids.insert(id);
        }
        check(ids.size() == 64, "concurrent request IDs collided");
    });
}

void idle_receiver_waits() {
    with_peer([](int fd) { reply(fd, request(fd)); }, [](const std::string& path) {
        edgerpc::EdgeRpcClient client(path, 50ms);
        std::this_thread::sleep_for(150ms);
        check(client.infer_sync({2}).output == std::vector<float>{5}, "unused receiver timed out");
    });
}

void concurrent_close_and_submit() {
    with_peer([](int fd) {
        edgerpc::Frame frame;
        try { while (edgerpc::receive_frame(fd, frame, 2s)) {} }
        catch (const edgerpc::ProtocolError&) {}
    }, [](const std::string& path) {
        edgerpc::EdgeRpcClient client(path, 2s, 128);
        std::promise<void> start;
        const auto gate = start.get_future().share();
        std::vector<std::future<void>> callers;
        for (int i = 0; i < 8; ++i) {
            callers.push_back(std::async(std::launch::async, [&] {
                gate.wait();
                std::vector<std::future<edgerpc::Response>> futures;
                for (int j = 0; j < 64; ++j) futures.push_back(client.infer_async({1}));
                for (auto& future : futures) { ready(future); must_fail([&] { future.get(); }); }
            }));
        }
        auto closer = std::async(std::launch::async, [&] { gate.wait(); client.close(); });
        start.set_value();
        for (auto& caller : callers) { ready(caller); caller.get(); }
        ready(closer);
        closer.get();
    });
}

void bounded_pool_and_shutdown() {
    std::promise<void> started, release;
    auto running = started.get_future();
    auto gate = release.get_future();
    edgerpc::ThreadPool pool(1, 1);
    check(pool.try_submit([&] { started.set_value(); gate.wait_for(3s); }), "first task rejected");
    ready(running);
    auto queued = std::make_shared<std::promise<void>>();
    auto cancelled = queued->get_future();
    check(pool.try_submit([queued] { queued->set_value(); }), "waiting task rejected");
    queued.reset();
    check(!pool.try_submit([] {}), "full server queue accepted more work");
    auto stopping = std::async(std::launch::async, [&] { pool.shutdown(); });
    ready(cancelled);
    must_fail([&] { cancelled.get(); }); // Broken promise proves the queued closure was discarded.
    release.set_value();
    ready(stopping);
    stopping.get();
    check(!pool.try_submit([] {}), "stopped pool accepted work");
    must_fail([] { edgerpc::ThreadPool invalid(0, 1); });
    must_fail([] { edgerpc::ThreadPool invalid(1, 0); });
}

void worker_survives_task_exception() {
    edgerpc::ThreadPool pool(1, 2);
    std::promise<void> done;
    auto result = done.get_future();
    check(pool.try_submit([] { throw std::runtime_error("task failure"); }), "task rejected");
    check(pool.try_submit([&] { done.set_value(); }), "follow-up task rejected");
    ready(result);
    result.get();
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> cases{
        {"reversed responses", reversed_responses}, {"client backpressure", client_backpressure},
        {"reversed shared responses", reversed_shared_responses}, {"mixed transport responses", mixed_transport_responses},
        {"shared close cleanup", shared_close_cleanup}, {"shared disconnect cleanup", shared_disconnect_cleanup},
#ifdef __linux__
        {"failed SHM preparation preserves connection", failed_shared_preparation_preserves_connection},
#endif
        {"disconnect settles all", disconnect_settles_all}, {"close settles queued work", close_settles_queued_work},
        {"destruction settles futures", destruction_settles_futures}, {"timeout settles all", timeout_settles_all},
        {"protocol failure settles all", protocol_failure_settles_all}, {"remote errors isolated", remote_error_isolated},
        {"concurrent callers", concurrent_callers}, {"idle receiver waits", idle_receiver_waits},
        {"concurrent close and submit", concurrent_close_and_submit},
        {"bounded pool and shutdown", bounded_pool_and_shutdown}, {"worker survives task exception", worker_survives_task_exception}};
    std::size_t failures = 0;
    for (const auto& item : cases) {
        try { item.second(); std::cout << "PASS " << item.first << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << item.first << ": " << error.what() << '\n';
        }
    }
    std::cout << "cases=" << cases.size() << " failed=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
