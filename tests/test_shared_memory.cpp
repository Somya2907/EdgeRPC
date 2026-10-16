#include "edgerpc/shared_buffer.hpp"

#include <array>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <utility>
#include <unistd.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void must_fail(Function function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    check(failed, "expected rejection");
}

edgerpc::SharedDescriptor descriptor() {
    return {"/erpc_0123456789abcdef01234567", 32, 0, 16, 16, 4};
}

void descriptor_wire_format() {
    const auto d = descriptor();
    const auto bytes = edgerpc::encode_shared_descriptor(d);
    const std::vector<std::uint8_t> prefix{
        0,1, 0,30, 0,0,0,0,0,0,0,32, 0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,16, 0,0,0,16, 0,0,0,4, 0,0,0,0};
    check(bytes.size() == 70 && std::equal(prefix.begin(), prefix.end(), bytes.begin()), "descriptor byte order changed");
    check(std::string(bytes.begin() + 40, bytes.end()) == d.name, "descriptor name changed");
    const auto decoded = edgerpc::decode_shared_descriptor(bytes);
    check(decoded.region_bytes == 32 && decoded.input_offset == 0 && decoded.output_offset == 16 &&
          decoded.tensor_bytes == 16 && decoded.elements == 4 && decoded.name == d.name, "descriptor round trip failed");
    edgerpc::Header request;
    request.type = edgerpc::MessageType::ShmRequest;
    request.payload_size = 70;
    check(edgerpc::decode_header(edgerpc::encode_header(request)).type == request.type, "SHM type not accepted");
    request.payload_size = 71;
    must_fail([&] { edgerpc::encode_header(request); });
    request.type = edgerpc::MessageType::ShmResponse;
    request.payload_size = 0;
    check(edgerpc::decode_header(edgerpc::encode_header(request)).payload_size == 0, "completion must be empty");
    request.payload_size = 4;
    must_fail([&] { edgerpc::encode_header(request); });
}

void malformed_descriptors() {
    auto good = edgerpc::encode_shared_descriptor(descriptor());
    for (const auto index : {1, 3, 39, 40, 69}) {
        auto bad = good;
        bad[index] = 255;
        must_fail([&] { edgerpc::decode_shared_descriptor(bad); });
    }
    good.pop_back();
    must_fail([&] { edgerpc::decode_shared_descriptor(good); });
    for (const auto& name : {"", "/other_0123456789abcdef01234567", "/erpc_../0123456789abcdef01234",
                             "/erpc_0123456789abcdef0123456G"})
        must_fail([&] { edgerpc::validate_shared_name(name); });
}

void descriptor_bounds() {
    for (const auto& mutate : std::vector<std::function<void(edgerpc::SharedDescriptor&)>>{
        [](auto& d) { d.region_bytes = 0; }, [](auto& d) { d.region_bytes = edgerpc::kMaxSharedBytes + 1; },
        [](auto& d) { d.elements = edgerpc::kMaxElements + 1; }, [](auto& d) { d.tensor_bytes = 15; },
        [](auto& d) { d.input_offset = 1; }, [](auto& d) { d.output_offset = 20; },
        [](auto& d) { d.output_offset = 0; }, [](auto& d) { d.input_offset = std::numeric_limits<std::uint64_t>::max() - 3; }}) {
        auto d = descriptor();
        mutate(d);
        must_fail([&] { edgerpc::encode_shared_descriptor(d); });
    }
    auto d = descriptor();
    d.region_bytes = edgerpc::kMaxSharedBytes;
    d.output_offset = edgerpc::kMaxPayloadBytes;
    d.elements = edgerpc::kMaxElements;
    d.tensor_bytes = edgerpc::kMaxPayloadBytes;
    edgerpc::validate_shared_descriptor(d);
    d = {descriptor().name, 8, 8, 8, 0, 0};
    edgerpc::validate_shared_descriptor(d); // Empty ranges at the end are valid.
}

void independent_mappings_and_unlink() {
    auto owner = edgerpc::SharedBuffer::create(32);
    edgerpc::validate_shared_name(owner.name());
    owner.write_tensor(0, {-4, 0, 2.5f, 8});
    {
        auto attached = edgerpc::SharedBuffer::open(owner.name(), owner.size());
        check(attached.read_tensor(0, 4) == std::vector<float>({-4, 0, 2.5f, 8}), "independent mapping did not share input");
        attached.unlink_name();
        must_fail([&] { edgerpc::SharedBuffer::open(owner.name(), owner.size()); });
        attached.write_tensor(16, {-7, 1, 6, 17});
        check(owner.read_tensor(16, 4) == std::vector<float>({-7, 1, 6, 17}), "unlinked mappings lost shared storage");
    }
    check(owner.read_tensor(16, 4).back() == 17, "attached destructor invalidated owner mapping");
}

void ownership_and_moves() {
    std::string name;
    std::size_t bytes = 0;
    {
        auto owner = edgerpc::SharedBuffer::create(16);
        name = owner.name();
        bytes = owner.size();
        {
            auto attached = edgerpc::SharedBuffer::open(name, bytes);
            attached.write_tensor(0, {3});
        }
        auto still_named = edgerpc::SharedBuffer::open(name, bytes);
        auto moved = std::move(owner);
        must_fail([&] { owner.read_tensor(0, 1); });
        check(moved.read_tensor(0, 1) == std::vector<float>{3}, "move lost mapping");
        auto replacement = edgerpc::SharedBuffer::create(8);
        const auto obsolete = replacement.name();
        const auto obsolete_bytes = replacement.size();
        replacement = std::move(moved);
        must_fail([&] { edgerpc::SharedBuffer::open(obsolete, obsolete_bytes); });
        check(replacement.read_tensor(0, 1) == std::vector<float>{3}, "move assignment lost mapping");
    }
    must_fail([&] { edgerpc::SharedBuffer::open(name, bytes); });
}

void size_permissions_and_access() {
    auto region = edgerpc::SharedBuffer::create(32);
    must_fail([&] { edgerpc::SharedBuffer::open(region.name(), region.size() + 4); });
    int fd = ::shm_open(region.name().c_str(), O_RDWR, 0);
    check(fd >= 0, "test cannot inspect created object");
    struct stat info{};
    check(::fstat(fd, &info) == 0 && info.st_uid == ::geteuid() && (info.st_mode & 0777) == 0600,
          "new object must be same-user, mode 0600");
    ::close(fd);
    // Create an invalid-mode object directly: macOS PSXSHM does not support fchmod.
    auto permissions = edgerpc::SharedBuffer::create(32);
    const auto invalid_name = permissions.name();
    permissions.unlink_name();
    const auto previous_mask = ::umask(0); // This unit executable has no worker threads.
    fd = ::shm_open(invalid_name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0640);
    ::umask(previous_mask);
    check(fd >= 0, "test cannot create invalid-mode object");
    const int truncated = ::ftruncate(fd, 32);
    struct stat invalid_info{};
    const int inspected = ::fstat(fd, &invalid_info);
    ::close(fd);
    try {
        check(truncated == 0 && inspected == 0 && (invalid_info.st_mode & 0777) == 0640,
              "test cannot inspect invalid-mode object");
        must_fail([&] { edgerpc::SharedBuffer::open(invalid_name, invalid_info.st_size); });
    } catch (...) { ::shm_unlink(invalid_name.c_str()); throw; }
    ::shm_unlink(invalid_name.c_str());
    must_fail([&] { region.write_tensor(1, {1}); });
    must_fail([&] { region.write_tensor(region.size(), {1}); });
    must_fail([&] { region.read_tensor(std::numeric_limits<std::uint64_t>::max() - 3, 1); });
    must_fail([&] { region.read_tensor(0, edgerpc::kMaxElements + 1); });
    check(region.read_tensor(region.size(), 0).empty(), "zero-sized access at end should be valid");
    region.write_tensor(region.size(), {});
    must_fail([] { edgerpc::SharedBuffer::create(0); });
    must_fail([] { edgerpc::SharedBuffer::create(edgerpc::kMaxSharedBytes + 1); });
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> cases{
        {"descriptor wire format", descriptor_wire_format}, {"malformed descriptors", malformed_descriptors},
        {"descriptor bounds", descriptor_bounds}, {"independent mappings and unlink", independent_mappings_and_unlink},
        {"ownership and moves", ownership_and_moves}, {"size permissions and access", size_permissions_and_access}};
    std::size_t failures = 0;
    for (const auto& item : cases) {
        try { item.second(); std::cout << "PASS " << item.first << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << item.first << ": " << error.what() << '\n'; }
    }
    std::cout << "cases=" << cases.size() << " failed=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
