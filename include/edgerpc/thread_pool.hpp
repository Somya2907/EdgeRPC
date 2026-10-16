#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace edgerpc {

// Capacity bounds waiting tasks; up to workers additional tasks may be executing.
class ThreadPool {
public:
    ThreadPool(std::size_t workers, std::size_t capacity);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    bool try_submit(std::function<void()> task);
    // Discard waiting tasks, finish running tasks, then join. Call from the owner.
    void shutdown();
private:
    void work();
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopped_ = false;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
};

} // namespace edgerpc
