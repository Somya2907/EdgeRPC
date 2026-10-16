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
        header.type != MessageType::Error && header.type != MessageType::ShmRequest &&
        header.type != MessageType::ShmResponse && header.type != MessageType::RegisterBuffer &&
        header.type != MessageType::RegisterAck && header.type != MessageType::PooledRequest &&
        header.type != MessageType::PooledResponse) throw ProtocolError("unknown message type");
    if (header.payload_size > kMaxPayloadBytes) throw ProtocolError("payload exceeds 8 MiB limit");
    if (header.type == MessageType::ShmRequest && header.payload_size != kSharedDescriptorBytes)
        throw ProtocolError("SHM request must contain one descriptor");
    if (header.type == MessageType::ShmResponse && header.payload_size != 0)
        throw ProtocolError("SHM completion must have no payload");
    if ((header.type == MessageType::RegisterBuffer && header.payload_size != 74) ||
        (header.type == MessageType::RegisterAck && header.payload_size != 4) ||
        (header.type == MessageType::PooledRequest && header.payload_size != 24) ||
        (header.type == MessageType::PooledResponse && header.payload_size != 48))
        throw ProtocolError("invalid registered-buffer control length");
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

void validate_shared_name(const std::string& name) {
    if (name.size() != kSharedNameBytes || name.compare(0, 6, "/erpc_") != 0 ||
        name.find_first_not_of("0123456789abcdef", 6) != std::string::npos)
        throw ProtocolError("invalid EdgeRPC shared-memory name");
}

void validate_shared_descriptor(const SharedDescriptor& descriptor) {
    validate_shared_name(descriptor.name);
    if (descriptor.region_bytes < 8 || descriptor.region_bytes > kMaxSharedBytes)
        throw ProtocolError("shared region must be 8 bytes through 16 MiB");
    if (descriptor.elements > kMaxElements || descriptor.tensor_bytes != descriptor.elements * 4ULL)
        throw ProtocolError("shared tensor count/length mismatch");
    const auto valid_range = [&](std::uint64_t offset) {
        return offset % 4 == 0 && offset <= descriptor.region_bytes &&
               descriptor.tensor_bytes <= descriptor.region_bytes - offset;
    };
    if (!valid_range(descriptor.input_offset) || !valid_range(descriptor.output_offset))
        throw ProtocolError("shared tensor range is unaligned or outside region");
    if (descriptor.tensor_bytes != 0 &&
        descriptor.input_offset < descriptor.output_offset + descriptor.tensor_bytes &&
        descriptor.output_offset < descriptor.input_offset + descriptor.tensor_bytes)
        throw ProtocolError("shared input and output ranges overlap");
}

std::vector<std::uint8_t> encode_shared_descriptor(const SharedDescriptor& descriptor) {
    validate_shared_descriptor(descriptor);
    std::vector<std::uint8_t> bytes(kSharedDescriptorBytes);
    auto* out = bytes.data();
    put(out, 1, 2); // Descriptor version, independent of the frame version.
    put(out, kSharedNameBytes, 2);
    put(out, descriptor.region_bytes, 8);
    put(out, descriptor.input_offset, 8);
    put(out, descriptor.output_offset, 8);
    put(out, descriptor.tensor_bytes, 4);
    put(out, descriptor.elements, 4);
    put(out, 0, 4); // Reserved; reject nonzero values rather than guessing semantics.
    std::memcpy(out, descriptor.name.data(), kSharedNameBytes);
    return bytes;
}

SharedDescriptor decode_shared_descriptor(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != kSharedDescriptorBytes) throw ProtocolError("invalid SHM descriptor length");
    const auto* in = bytes.data();
    if (get(in, 2) != 1 || get(in, 2) != kSharedNameBytes)
        throw ProtocolError("unsupported SHM descriptor version/name length");
    SharedDescriptor descriptor;
    descriptor.region_bytes = get(in, 8);
    descriptor.input_offset = get(in, 8);
    descriptor.output_offset = get(in, 8);
    descriptor.tensor_bytes = static_cast<std::uint32_t>(get(in, 4));
    descriptor.elements = static_cast<std::uint32_t>(get(in, 4));
    if (get(in, 4) != 0) throw ProtocolError("nonzero SHM reserved field");
    descriptor.name.assign(reinterpret_cast<const char*>(in), kSharedNameBytes);
    validate_shared_descriptor(descriptor);
    return descriptor;
}

std::vector<std::uint8_t> encode_slot_id(std::uint32_t slot) {
    if (slot < 1 || slot > 4) throw ProtocolError("slot ID must be 1..4");
    std::vector<std::uint8_t> bytes(4);
    auto* out = bytes.data();
    put(out, slot, 4);
    return bytes;
}

std::uint32_t decode_slot_id(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 4) throw ProtocolError("invalid slot ID length");
    const auto* in = bytes.data();
    const auto slot = static_cast<std::uint32_t>(get(in, 4));
    if (slot < 1 || slot > 4) throw ProtocolError("slot ID must be 1..4");
    return slot;
}

std::vector<std::uint8_t> encode_registration(const BufferRegistration& registration) {
    auto bytes = encode_slot_id(registration.slot);
    const auto descriptor = encode_shared_descriptor(registration.descriptor);
    bytes.insert(bytes.end(), descriptor.begin(), descriptor.end());
    return bytes;
}

BufferRegistration decode_registration(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 74) throw ProtocolError("invalid buffer registration length");
    return {decode_slot_id({bytes.begin(), bytes.begin() + 4}),
            decode_shared_descriptor({bytes.begin() + 4, bytes.end()})};
}

std::vector<std::uint8_t> encode_slot_reference(const SlotReference& reference) {
    if (reference.slot < 1 || reference.slot > 4 || reference.elements > kMaxElements || reference.generation == 0)
        throw ProtocolError("invalid slot reference");
    std::vector<std::uint8_t> bytes(24);
    auto* out = bytes.data();
    put(out, reference.slot, 4);
    put(out, reference.elements, 4);
    put(out, reference.generation, 8);
    put(out, 0, 8);
    return bytes;
}

SlotReference decode_slot_reference(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 24) throw ProtocolError("invalid slot reference length");
    const auto* in = bytes.data();
    SlotReference reference;
    reference.slot = static_cast<std::uint32_t>(get(in, 4));
    reference.elements = static_cast<std::uint32_t>(get(in, 4));
    reference.generation = get(in, 8);
    if (get(in, 8) != 0 || reference.slot < 1 || reference.slot > 4 ||
        reference.elements > kMaxElements || reference.generation == 0) throw ProtocolError("invalid slot reference");
    return reference;
}

std::vector<std::uint8_t> encode_pool_completion(const PoolCompletion& completion) {
    if (completion.queued_ns == 0 || completion.execute_begin_ns < completion.queued_ns ||
        completion.execute_end_ns < completion.execute_begin_ns) throw ProtocolError("invalid pool stage timestamps");
    auto bytes = encode_slot_reference(completion.reference);
    bytes.resize(48);
    auto* out = bytes.data() + 24;
    put(out, completion.queued_ns, 8);
    put(out, completion.execute_begin_ns, 8);
    put(out, completion.execute_end_ns, 8);
    return bytes;
}

PoolCompletion decode_pool_completion(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 48) throw ProtocolError("invalid pool completion length");
    PoolCompletion completion;
    completion.reference = decode_slot_reference({bytes.begin(), bytes.begin() + 24});
    const auto* in = bytes.data() + 24;
    completion.queued_ns = get(in, 8);
    completion.execute_begin_ns = get(in, 8);
    completion.execute_end_ns = get(in, 8);
    if (completion.queued_ns == 0 || completion.execute_begin_ns < completion.queued_ns ||
        completion.execute_end_ns < completion.execute_begin_ns) throw ProtocolError("invalid pool stage timestamps");
    return completion;
}

} // namespace edgerpc
