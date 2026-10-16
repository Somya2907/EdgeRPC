#include "edgerpc/metrics.hpp"
#include "edgerpc/mock_executor.hpp"
#include "edgerpc/transport.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <sys/socket.h>
#include <thread>
#include <utility>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void must_throw(Function function) {
    bool threw = false;
    try { function(); } catch (const std::exception&) { threw = true; }
    check(threw, "expected an exception");
}

std::array<edgerpc::UniqueFd, 2> sockets() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) throw std::runtime_error("socketpair failed");
    return {edgerpc::UniqueFd(fds[0]), edgerpc::UniqueFd(fds[1])};
}

void header_wire_format() {
    edgerpc::Header header;
    header.request_id = 0x0102030405060708ULL;
    header.payload_size = 4;
    const std::array<std::uint8_t, edgerpc::kHeaderBytes> expected{
        0x45,0x52,0x50,0x43, 0,1, 0,1, 1,2,3,4,5,6,7,8, 0,0,0,4, 0,0,0,0, 0,0,0,1};
    check(edgerpc::encode_header(header) == expected, "header must use documented bytes");
    const auto decoded = edgerpc::decode_header(expected);
    check(decoded.request_id == header.request_id && decoded.payload_size == 4, "header round trip failed");
    header.type = edgerpc::MessageType::Error;
    header.status = edgerpc::Status::InvalidRequest;
    header.request_id = std::numeric_limits<std::uint64_t>::max();
    check(edgerpc::decode_header(edgerpc::encode_header(header)).request_id == header.request_id,
          "64-bit request ID was truncated");
}

void malformed_headers() {
    const auto good = edgerpc::encode_header(edgerpc::Header{});
    for (const auto index : {0, 5, 7}) {
        auto bad = good;
        bad[index] = 255;
        must_throw([&] { edgerpc::decode_header(bad); });
    }
    auto huge = good;
    huge[16] = 0xff;
    must_throw([&] { edgerpc::decode_header(huge); });
    edgerpc::Header max;
    max.payload_size = edgerpc::kMaxPayloadBytes;
    check(edgerpc::decode_header(edgerpc::encode_header(max)).payload_size == max.payload_size,
          "maximum payload rejected");
    ++max.payload_size;
    must_throw([&] { edgerpc::encode_header(max); });
}

void tensor_wire_format() {
    const std::vector<float> input{1.0f, -2.5f, -0.0f, std::numeric_limits<float>::infinity()};
    const std::vector<std::uint8_t> expected{
        0x3f,0x80,0,0, 0xc0,0x20,0,0, 0x80,0,0,0, 0x7f,0x80,0,0};
    check(edgerpc::encode_tensor(input) == expected, "float32 wire order is wrong");
    const auto output = edgerpc::decode_tensor(expected);
    check(std::memcmp(input.data(), output.data(), expected.size()) == 0, "float bits changed");
    check(edgerpc::decode_tensor({}).empty(), "empty tensor is valid");
    must_throw([] { edgerpc::decode_tensor({1, 2, 3}); });
    must_throw([] { edgerpc::encode_tensor(std::vector<float>(edgerpc::kMaxElements + 1)); });
}

void fragmented_frames() {
    auto pair = sockets();
    edgerpc::Frame original;
    original.header.request_id = 42;
    original.payload = edgerpc::encode_tensor({-3, 0, 4});
    original.header.payload_size = static_cast<std::uint32_t>(original.payload.size());
    auto writer = std::async(std::launch::async, [&] {
        const auto header = edgerpc::encode_header(original.header);
        for (auto byte : header) {
            edgerpc::write_all(pair[0].get(), &byte, 1);
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
        for (auto byte : original.payload) edgerpc::write_all(pair[0].get(), &byte, 1);
        // A second frame on the same byte stream must remain separately readable.
        edgerpc::send_frame(pair[0].get(), original);
    });
    for (int i = 0; i < 2; ++i) {
        edgerpc::Frame received;
        check(edgerpc::receive_frame(pair[1].get(), received), "unexpected EOF");
        check(received.header.request_id == 42 && received.payload == original.payload, "fragmented frame changed");
    }
    writer.get();
}

void large_partial_writes() {
    auto pair = sockets();
    int size = 1024;
    check(::setsockopt(pair[0].get(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0, "setsockopt failed");
    std::vector<std::uint8_t> payload(1024 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i % 251);
    auto writer = std::async(std::launch::async, [&] {
        edgerpc::write_all(pair[0].get(), payload.data(), payload.size());
    });
    std::vector<std::uint8_t> received(payload.size());
    check(edgerpc::read_exact(pair[1].get(), received.data(), received.size()), "missing large payload");
    writer.get();
    check(received == payload, "partial write lost bytes");
}

void eof_and_disconnect() {
    {
        auto pair = sockets();
        pair[0].reset();
        edgerpc::Frame received;
        check(!edgerpc::receive_frame(pair[1].get(), received), "clean EOF should return false");
        const char data = 'x';
        must_throw([&] { edgerpc::write_all(pair[1].get(), &data, 1); }); // Must not SIGPIPE.
    }
    for (const bool missing_body : {false, true}) {
        auto pair = sockets();
        edgerpc::Header header;
        header.payload_size = 4;
        const auto bytes = edgerpc::encode_header(header);
        edgerpc::write_all(pair[0].get(), bytes.data(), missing_body ? bytes.size() : 3);
        pair[0].reset();
        edgerpc::Frame received;
        must_throw([&] { edgerpc::receive_frame(pair[1].get(), received); });
    }
}

void timeouts_and_validation() {
    auto pair = sockets();
    char byte{};
    must_throw([&] { edgerpc::read_exact(pair[0].get(), &byte, 1, edgerpc::Timeout{20}); });
    must_throw([&] { edgerpc::read_exact(pair[0].get(), &byte, 1, edgerpc::Timeout{0}); });
    const std::vector<std::uint8_t> blocked(1024 * 1024);
    must_throw([&] {
        edgerpc::write_all(pair[0].get(), blocked.data(), blocked.size(), edgerpc::Timeout{20});
    });
    edgerpc::Frame inconsistent;
    inconsistent.header.payload_size = 4;
    must_throw([&] { edgerpc::send_frame(pair[0].get(), inconsistent); });
    must_throw([] { edgerpc::connect_unix(""); });
    must_throw([] { edgerpc::connect_unix(std::string(200, 'x')); });
}

void mock_backend() {
    edgerpc::MockExecutor executor;
    const std::vector<float> input{-4, 0, 3.5};
    check(executor.execute(input, edgerpc::Operation::Compute) == std::vector<float>({-7, 1, 8}),
          "mock compute failed");
    check(executor.execute(input, edgerpc::Operation::Noop) == input, "noop must echo values");
    check(executor.execute({}, edgerpc::Operation::Compute).empty(), "empty compute failed");
    must_throw([&] { executor.execute(input, static_cast<edgerpc::Operation>(99)); });
}

void percentile_metrics() {
    const auto summary = edgerpc::summarize_latencies({100, 1, 2, 3, 4});
    check(summary.p50_us == 3 && summary.p95_us == 100 && summary.p99_us == 100,
          "nearest-rank percentiles are wrong");
    const auto single = edgerpc::summarize_latencies({7});
    check(single.p50_us == 7 && single.p99_us == 7, "single sample failed");
    must_throw([] { edgerpc::summarize_latencies({}); });
}

void timed_trace_contract() {
    const auto bytes = edgerpc::encode_execution_trace({1, 256, 65536});
    check(bytes.size() == 24 && bytes[7] == 1 && bytes[14] == 1 && bytes[21] == 1, "trace byte order");
    check(edgerpc::decode_execution_trace(bytes).execute_end_ns == 65536, "trace round trip");
    for (const auto trace : std::vector<edgerpc::ExecutionTrace>{{0,1,2}, {2,1,3}, {1,3,2}})
        must_throw([&] { edgerpc::encode_execution_trace(trace); });
    auto bad = bytes;
    bad[7] = 255; bad[15] = 0; bad[14] = 0;
    must_throw([&] { edgerpc::decode_execution_trace(bad); });
    bad.pop_back();
    must_throw([&] { edgerpc::decode_execution_trace(bad); });
    edgerpc::Header header;
    header.type = edgerpc::MessageType::TimedResponse;
    for (const auto size : {24U, edgerpc::kMaxPayloadBytes + 24}) {
        header.payload_size = size;
        edgerpc::encode_header(header);
    }
    for (const auto size : {0U,23U,25U,edgerpc::kMaxPayloadBytes + 28}) {
        header.payload_size = size;
        must_throw([&] { edgerpc::encode_header(header); });
    }
    header.type = edgerpc::MessageType::TimedShmResponse;
    header.payload_size = 24;
    edgerpc::encode_header(header);
    header.payload_size = 28;
    must_throw([&] { edgerpc::encode_header(header); });
}

void measured_socket_traffic() {
    auto pair = sockets();
    edgerpc::IoCounters writer, reader;
    const char data[] = "abc";
    edgerpc::write_all(pair[0].get(), data, 3, edgerpc::kDefaultTimeout, &writer);
    pair[0].reset();
    char buffer[8]{};
    must_throw([&] { edgerpc::read_exact(pair[1].get(), buffer, sizeof(buffer), edgerpc::kDefaultTimeout, &reader); });
    check(writer.snapshot().sent == 3 && reader.snapshot().received == 3, "partial failed I/O must be counted");
    check(writer.snapshot().received == 0 && reader.snapshot().sent == 0, "byte directions mixed");
    auto blocked = sockets();
    edgerpc::IoCounters partial;
    const std::vector<std::uint8_t> large(1024 * 1024);
    must_throw([&] { edgerpc::write_all(blocked[0].get(), large.data(), large.size(), edgerpc::Timeout{20}, &partial); });
    check(partial.snapshot().sent > 0 && partial.snapshot().sent < large.size(), "partial write counter lost progress");
    ::shutdown(blocked[0].get(), SHUT_WR);
    std::size_t drained = 0;
    for (;;) {
        const auto count = ::recv(blocked[1].get(), buffer, sizeof(buffer), 0);
        check(count >= 0, "drain failed");
        if (count == 0) break;
        drained += count;
    }
    check(drained == partial.snapshot().sent, "counter differs from actual delivered bytes");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> cases{
        {"header wire format", header_wire_format}, {"malformed headers", malformed_headers},
        {"tensor wire format", tensor_wire_format}, {"fragmented frames", fragmented_frames},
        {"large partial writes", large_partial_writes}, {"EOF and disconnect", eof_and_disconnect},
        {"timeouts and validation", timeouts_and_validation}, {"mock backend", mock_backend},
        {"percentile metrics", percentile_metrics}, {"timed trace contract", timed_trace_contract},
        {"measured socket traffic", measured_socket_traffic}};
    std::size_t failures = 0;
    for (const auto& item : cases) {
        std::cout << "RUN " << item.first << std::endl;
        try { item.second(); std::cout << "PASS " << item.first << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << item.first << ": " << error.what() << '\n';
        }
    }
    std::cout << "cases=" << cases.size() << " failed=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
