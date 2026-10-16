#pragma once

#include "edgerpc/shared_buffer.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>

namespace edgerpc {

enum class BufferState { Free, Filling, InFlight, Ready };
struct BufferLease {
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<SharedBuffer> buffer;
    std::uint64_t output_offset = 0;
};

class BufferPool {
public:
    BufferPool(std::size_t slots, std::size_t max_elements);
    ~BufferPool();
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferLease acquire(); // Wait for a slot in the fixed pool; stop() wakes all waiters.
    void mark_in_flight(const BufferLease& lease);
    void mark_ready(const BufferLease& lease);
    void release(const BufferLease& lease); // Only FILLING or READY; never an executing slot.
    void stop();
    BufferState state(std::uint32_t slot) const;
    std::size_t size() const noexcept { return slots_.size(); }
    std::size_t capacity_elements() const noexcept { return max_elements_; }
    SharedDescriptor descriptor(std::uint32_t slot) const;
private:
    struct Slot {
        std::shared_ptr<SharedBuffer> buffer;
        SharedDescriptor descriptor;
        BufferState state = BufferState::Free;
        std::uint64_t generation = 0;
    };
    Slot& checked(const BufferLease& lease);
    const std::size_t max_elements_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool stopped_ = false;
    std::size_t next_ = 0;
    std::vector<Slot> slots_;
};

} // namespace edgerpc
