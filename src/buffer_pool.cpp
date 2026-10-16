#include "edgerpc/buffer_pool.hpp"

#include <algorithm>
#include <limits>

namespace edgerpc {

BufferPool::BufferPool(std::size_t slots, std::size_t max_elements) : max_elements_(max_elements) {
    if ((slots != 1 && slots != 2 && slots != 4) || max_elements > kMaxElements)
        throw std::invalid_argument("pool requires 1, 2, or 4 slots and at most 8 MiB per tensor");
    slots_.reserve(slots);
    const auto bytes = max_elements * 4;
    for (std::size_t i = 0; i < slots; ++i) {
        auto shared = std::make_shared<SharedBuffer>(SharedBuffer::create(std::max<std::size_t>(8, bytes * 2)));
        slots_.push_back({shared, {shared->name(), shared->size(), 0, bytes,
            static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(max_elements)}});
    }
}

BufferPool::~BufferPool() { stop(); }

BufferLease BufferPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] {
        return stopped_ || std::any_of(slots_.begin(), slots_.end(), [](const auto& slot) { return slot.state == BufferState::Free; });
    });
    if (stopped_) throw std::runtime_error("buffer pool stopped");
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto index = (next_ + i) % slots_.size();
        auto& slot = slots_[index];
        if (slot.state != BufferState::Free) continue;
        if (slot.generation == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("slot generations exhausted; create a new client");
        ++slot.generation;
        slot.state = BufferState::Filling;
        next_ = (index + 1) % slots_.size();
        return {static_cast<std::uint32_t>(index + 1), slot.generation, slot.buffer, slot.descriptor.output_offset};
    }
    throw std::logic_error("free buffer disappeared under lock");
}

BufferPool::Slot& BufferPool::checked(const BufferLease& lease) {
    if (lease.slot < 1 || lease.slot > slots_.size()) throw ProtocolError("unknown buffer lease");
    auto& slot = slots_[lease.slot - 1];
    if (slot.generation != lease.generation || slot.buffer != lease.buffer ||
        slot.descriptor.output_offset != lease.output_offset) throw ProtocolError("stale or invalid buffer lease");
    return slot;
}

void BufferPool::mark_in_flight(const BufferLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& slot = checked(lease);
    if (stopped_ || slot.state != BufferState::Filling) throw ProtocolError("buffer is not writable for publication");
    slot.state = BufferState::InFlight;
}

void BufferPool::mark_ready(const BufferLease& lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& slot = checked(lease);
    if (slot.state != BufferState::InFlight) throw ProtocolError("buffer was not in flight");
    slot.state = BufferState::Ready;
}

void BufferPool::release(const BufferLease& lease) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& slot = checked(lease);
        if (slot.state != BufferState::Filling && slot.state != BufferState::Ready)
            throw ProtocolError("cannot release a free or executing buffer");
        slot.state = BufferState::Free;
    }
    changed_.notify_one();
}

void BufferPool::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    changed_.notify_all();
}

BufferState BufferPool::state(std::uint32_t slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot < 1 || slot > slots_.size()) throw ProtocolError("unknown slot");
    return slots_[slot - 1].state;
}

SharedDescriptor BufferPool::descriptor(std::uint32_t slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot < 1 || slot > slots_.size()) throw ProtocolError("unknown slot");
    return slots_[slot - 1].descriptor;
}

} // namespace edgerpc
