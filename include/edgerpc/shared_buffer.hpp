#pragma once

#include "edgerpc/protocol.hpp"

namespace edgerpc {

// Mapped bytes are native IEEE-754 float32 on the same host/kernel namespace.
// The creator owns name cleanup; attached mappings never truncate or resize.
class SharedBuffer {
public:
    // At least bytes; size() is the actual OS size (macOS may round to a page).
    static SharedBuffer create(std::size_t bytes);
    static SharedBuffer open(const std::string& name, std::size_t expected_bytes);
    ~SharedBuffer();
    SharedBuffer(const SharedBuffer&) = delete;
    SharedBuffer& operator=(const SharedBuffer&) = delete;
    SharedBuffer(SharedBuffer&& other) noexcept;
    SharedBuffer& operator=(SharedBuffer&& other) noexcept;

    const std::string& name() const noexcept { return name_; }
    std::size_t size() const noexcept { return size_; }
    void write_tensor(std::uint64_t offset, const std::vector<float>& input);
    std::vector<float> read_tensor(std::uint64_t offset, std::size_t elements) const;
    // After attach, the server removes the name. Both existing mappings stay valid.
    void unlink_name();
private:
    SharedBuffer() = default;
    void reset() noexcept;
    void swap(SharedBuffer& other) noexcept;
    void check_range(std::uint64_t offset, std::size_t elements) const;
    std::string name_;
    std::size_t size_ = 0;
    void* mapping_ = nullptr;
    bool owns_name_ = false;
};

} // namespace edgerpc
