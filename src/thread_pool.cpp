#include "edgerpc/thread_pool.hpp"

#include <stdexcept>
#include <utility>

namespace edgerpc {

ThreadPool::ThreadPool(std::size_t workers, std::size_t capacity) : capacity_(capacity) {
    if (workers == 0 || workers > 16 || capacity == 0 || capacity > 1024)
        throw std::invalid_argument("workers must be 1..16 and queue capacity 1..1024");
    try {
        for (std::size_t i = 0; i < workers; ++i) workers_.emplace_back([this] { work(); });
    } catch (...) { shutdown(); throw; }
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::try_submit(std::function<void()> task) {
    if (!task) throw std::invalid_argument("task must not be empty");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(task));
    }
    changed_.notify_one();
    return true;
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        queue_.clear();
    }
    changed_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
}

void ThreadPool::work() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
            if (stopped_) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        // Runtime tasks handle/report execution failures; a task cannot kill a worker.
        try { task(); } catch (...) {}
    }
}

} // namespace edgerpc
