#include "pool/thread_pool.h"

namespace transcode {

void ThreadPool::start() {
    for (size_t i = 0; i < workers_.size(); ++i) {
        workers_[i] = std::thread([this]() {
            for (;;) {
                Task task;
                {
                    std::unique_lock<std::mutex> lk(mutex_);
                    cv_.wait(lk, [this]() { return stop_ || !tasks_.empty(); });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
}

void ThreadPool::stop() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (stop_) return;
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::enqueue(Task t) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        tasks_.push(std::move(t));
    }
    cv_.notify_one();
}

} // namespace transcode
