#include "edgerpc/protocol.hpp"

#include <cstring>
#include <limits>

namespace edgerpc {
namespace {

void put(std::uint8_t*& out, std::uint64_t value, unsigned width) {
    for (unsigned i = width; i > 0; --i) *out++ = static_cast<std::uint8_t>(value >> ((i - 1) * 8));
}

std::uint64_t get(const std::uint8_t*& in, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = (value << 8) | *in++;
    return value;
}

void validate(const Header& header) {
    if (header.type != MessageType::Request && header.type != MessageType::Response &&
        header.type != MessageType::Error) throw ProtocolError("unknown message type");
    if (header.payload_size > kMaxPayloadBytes) throw ProtocolError("payload exceeds 8 MiB limit");
}

} // namespace

std::array<std::uint8_t, kHeaderBytes> encode_header(const Header& header) {
    validate(header);
    std::array<std::uint8_t, kHeaderBytes> bytes{};
    auto* out = bytes.data();
    put(out, kMagic, 4);
    put(out, kVersion, 2);
    put(out, static_cast<std::uint16_t>(header.type), 2);
    put(out, header.request_id, 8);
    put(out, header.payload_size, 4);
    put(out, static_cast<std::uint32_t>(header.status), 4);
    put(out, header.flags, 4);
    return bytes;
}

Header decode_header(const std::array<std::uint8_t, kHeaderBytes>& bytes) {
    const auto* in = bytes.data();
    if (get(in, 4) != kMagic) throw ProtocolError("invalid frame magic");
    if (get(in, 2) != kVersion) throw ProtocolError("unsupported protocol version");
    Header header;
    header.type = static_cast<MessageType>(get(in, 2));
    header.request_id = get(in, 8);
    header.payload_size = static_cast<std::uint32_t>(get(in, 4));
    header.status = static_cast<Status>(get(in, 4));
    header.flags = static_cast<std::uint32_t>(get(in, 4));
    validate(header);
    return header;
}

std::vector<std::uint8_t> encode_tensor(const std::vector<float>& values) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "EdgeRPC requires IEEE-754 binary32 floats");
    if (values.size() > kMaxElements) throw ProtocolError("tensor exceeds 8 MiB limit");
    std::vector<std::uint8_t> bytes(values.size() * 4);
    auto* out = bytes.data();
    for (float value : values) {
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        put(out, bits, 4);
    }
    return bytes;
}

std::vector<float> decode_tensor(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > kMaxPayloadBytes || bytes.size() % 4 != 0)
        throw ProtocolError("tensor payload must be float32-aligned and at most 8 MiB");
    std::vector<float> values(bytes.size() / 4);
    const auto* in = bytes.data();
    for (float& value : values) {
        const auto bits = static_cast<std::uint32_t>(get(in, 4));
        std::memcpy(&value, &bits, sizeof(bits));
    }
    return values;
}

} // namespace edgerpc
