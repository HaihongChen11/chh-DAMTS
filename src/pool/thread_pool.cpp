#include "pool/thread_pool.h"

#include "common/logger.h"

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
                    cvFull_.notify_all();
                }
                try {
                    task();
                } catch (const std::exception& e) {
                    LOG_ERROR("thread pool task exception: {}", e.what());
                } catch (...) {
                    LOG_ERROR("thread pool task unknown exception");
                }
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
    cvFull_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::enqueue(Task t) {
    {
        std::unique_lock<std::mutex> lk(mutex_);
        cvFull_.wait(lk, [this]() { return stop_ || tasks_.size() < maxQueueSize_; });
        if (stop_) return;
        tasks_.push(std::move(t));
    }
    cv_.notify_one();
}

bool ThreadPool::tryEnqueue(Task t) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (stop_ || tasks_.size() >= maxQueueSize_) return false;
        tasks_.push(std::move(t));
    }
    cv_.notify_one();
    return true;
}

} // namespace transcode
