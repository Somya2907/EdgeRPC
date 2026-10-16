#include "edgerpc/buffer_pool.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <iostream>

namespace {
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, "invalid operation accepted");
}

void wire_contract() {
    const edgerpc::SlotReference ref{2, 4, 0x0102030405060708ULL};
    const std::vector<std::uint8_t> expected{
        0,0,0,2, 0,0,0,4, 1,2,3,4,5,6,7,8, 0,0,0,0,0,0,0,0};
    check(edgerpc::encode_slot_reference(ref) == expected, "slot reference byte order");
    check(edgerpc::decode_slot_reference(expected).generation == ref.generation, "generation round trip");
    auto reserved = expected;
    reserved.back() = 1;
    rejects([&] { edgerpc::decode_slot_reference(reserved); });
    auto short_ref = expected;
    short_ref.pop_back();
    rejects([&] { edgerpc::decode_slot_reference(short_ref); });
    for (const auto& bad : std::vector<edgerpc::SlotReference>{{0,4,1}, {5,4,1}, {1,4,0},
             {1,static_cast<std::uint32_t>(edgerpc::kMaxElements + 1),1}})
        rejects([&] { edgerpc::encode_slot_reference(bad); });
    const edgerpc::PoolCompletion done{ref, 11, 22, 33};
    const auto bytes = edgerpc::encode_pool_completion(done);
    check(bytes.size() == 48 && bytes[31] == 11 && bytes[39] == 22 && bytes[47] == 33,
          "completion byte order");
    check(edgerpc::decode_pool_completion(bytes).execute_end_ns == 33, "completion round trip");
    for (const auto& bad : std::vector<edgerpc::PoolCompletion>{{ref,0,1,2}, {ref,2,1,3}, {ref,1,3,2}})
        rejects([&] { edgerpc::encode_pool_completion(bad); });
    edgerpc::BufferPool pool(1, 4);
    const auto registration = edgerpc::encode_registration({1, pool.descriptor(1)});
    check(registration.size() == 74 && registration[3] == 1, "registration prefix");
    check(edgerpc::decode_registration(registration).descriptor.name == pool.descriptor(1).name,
          "registration round trip");
    check(edgerpc::encode_slot_id(4) == std::vector<std::uint8_t>({0,0,0,4}), "ack byte order");
    for (const auto& item : std::vector<std::pair<edgerpc::MessageType, std::uint32_t>>{
             {edgerpc::MessageType::RegisterBuffer,74}, {edgerpc::MessageType::RegisterAck,4},
             {edgerpc::MessageType::PooledRequest,24}, {edgerpc::MessageType::PooledResponse,48}}) {
        edgerpc::Header header;
        header.type = item.first;
        header.payload_size = item.second;
        check(edgerpc::decode_header(edgerpc::encode_header(header)).type == item.first, "header round trip");
        ++header.payload_size;
        rejects([&] { edgerpc::encode_header(header); });
    }
}

void ownership_and_generations() {
    edgerpc::BufferPool pool(2, 4);
    const auto a = pool.acquire(), b = pool.acquire();
    check(a.slot != b.slot && a.generation == 1 && b.generation == 1, "initial ownership");
    check(pool.state(a.slot) == edgerpc::BufferState::Filling, "acquire must enter FILLING");
    pool.mark_in_flight(a);
    rejects([&] { pool.release(a); });
    rejects([&] { pool.mark_in_flight(a); });
    rejects([&] { pool.mark_ready(b); });
    auto waiter = std::async(std::launch::async, [&] { return pool.acquire(); });
    const bool blocked = waiter.wait_for(20ms) == std::future_status::timeout;
    pool.mark_ready(a);
    pool.release(a);
    check(waiter.wait_for(2s) == std::future_status::ready, "release did not wake waiter");
    const auto next = waiter.get();
    check(blocked && next.slot == a.slot && next.generation == 2, "executing slot reused early");
    rejects([&] { pool.mark_in_flight(a); });
    auto forged = next;
    ++forged.output_offset;
    rejects([&] { pool.release(forged); });
    pool.release(next);
    rejects([&] { pool.release(next); });
    pool.release(b);
}

void stop_wakes_waiters() {
    edgerpc::BufferPool pool(1, 0);
    const auto lease = pool.acquire();
    pool.mark_in_flight(lease);
    auto waiter = std::async(std::launch::async, [&] { rejects([&] { pool.acquire(); }); });
    pool.stop();
    check(waiter.wait_for(2s) == std::future_status::ready, "stop did not interrupt acquire");
    waiter.get();
    check(pool.state(lease.slot) == edgerpc::BufferState::InFlight, "stop reclaimed executing storage");
    pool.mark_ready(lease);
    pool.release(lease);
    rejects([&] { pool.acquire(); });
}

void limits_and_cleanup() {
    for (const auto count : {0, 3, 5}) rejects([&] { edgerpc::BufferPool pool(count, 4); });
    rejects([] { edgerpc::BufferPool pool(1, edgerpc::kMaxElements + 1); });
    std::vector<edgerpc::SharedDescriptor> descriptors;
    {
        edgerpc::BufferPool pool(4, 4);
        for (std::uint32_t slot = 1; slot <= 4; ++slot) descriptors.push_back(pool.descriptor(slot));
        rejects([&] { pool.descriptor(0); });
        rejects([&] { pool.state(5); });
    }
    for (const auto& descriptor : descriptors)
        rejects([&] { edgerpc::SharedBuffer::open(descriptor.name, descriptor.region_bytes); });
}
}

int main() {
    unsigned failures = 0;
    for (const auto& test : std::vector<std::pair<const char*, std::function<void()>>>{
             {"wire contract",wire_contract}, {"ownership and generations",ownership_and_generations},
             {"stop wakes waiters",stop_wakes_waiters}, {"limits and cleanup",limits_and_cleanup}}) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << test.first << ": " << error.what() << '\n'; }
    }
    std::cout << "cases=4 failed=" << failures << '\n';
    return failures ? 1 : 0;
}
