#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace edgerpc {

constexpr std::uint32_t kMagic = 0x45525043; // ERPC
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 28;
constexpr std::uint32_t kMaxPayloadBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxElements = kMaxPayloadBytes / 4;

enum class MessageType : std::uint16_t { Request = 1, Response = 2, Error = 3 };
enum class Operation : std::uint32_t { Noop = 0, Compute = 1 };
enum class Status : std::uint32_t { Ok = 0, InvalidRequest = 1, InternalError = 2 };

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

std::array<std::uint8_t, kHeaderBytes> encode_header(const Header& header);
Header decode_header(const std::array<std::uint8_t, kHeaderBytes>& bytes);
std::vector<std::uint8_t> encode_tensor(const std::vector<float>& values);
std::vector<float> decode_tensor(const std::vector<std::uint8_t>& bytes);

} // namespace edgerpc
