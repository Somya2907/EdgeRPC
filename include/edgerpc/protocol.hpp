#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace edgerpc {

constexpr std::uint32_t kMagic = 0x45525043; // ERPC
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 28;
constexpr std::uint32_t kMaxPayloadBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxElements = kMaxPayloadBytes / 4;

enum class MessageType : std::uint16_t { Request = 1, Response = 2, Error = 3, ShmRequest = 4, ShmResponse = 5 };
enum class Operation : std::uint32_t { Noop = 0, Compute = 1 };
enum class TensorTransport { Socket, SharedMemory };
enum class Status : std::uint32_t { Ok = 0, InvalidRequest = 1, InternalError = 2, Overloaded = 3 };

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Header {
    MessageType type = MessageType::Request;
    std::uint64_t request_id = 0;
    std::uint32_t payload_size = 0;
    Status status = Status::Ok;
    std::uint32_t flags = static_cast<std::uint32_t>(Operation::Compute);
};

struct Frame {
    Header header;
    std::vector<std::uint8_t> payload;
};

constexpr std::uint64_t kMaxSharedBytes = 2ULL * kMaxPayloadBytes;
constexpr std::size_t kSharedNameBytes = 30; // /erpc_ followed by 24 lowercase hex digits
constexpr std::size_t kSharedDescriptorBytes = 40 + kSharedNameBytes;

struct SharedDescriptor {
    std::string name;
    std::uint64_t region_bytes = 0;
    std::uint64_t input_offset = 0;
    std::uint64_t output_offset = 0;
    std::uint32_t tensor_bytes = 0;
    std::uint32_t elements = 0;
};

void validate_shared_name(const std::string& name);
void validate_shared_descriptor(const SharedDescriptor& descriptor);
std::vector<std::uint8_t> encode_shared_descriptor(const SharedDescriptor& descriptor);
SharedDescriptor decode_shared_descriptor(const std::vector<std::uint8_t>& bytes);

std::array<std::uint8_t, kHeaderBytes> encode_header(const Header& header);
Header decode_header(const std::array<std::uint8_t, kHeaderBytes>& bytes);
std::vector<std::uint8_t> encode_tensor(const std::vector<float>& values);
std::vector<float> decode_tensor(const std::vector<std::uint8_t>& bytes);

} // namespace edgerpc
