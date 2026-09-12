#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/noncopyable.h"

namespace transcode {

// 手写业务线程池：任务队列 + 固定工作线程。
// 用途：把数据库、MQ、IO 等阻塞操作从 epoll 主循环剥离，防止阻塞 Reactor。
class ThreadPool : Noncopyable {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t threads) : workers_(threads) {}
    ~ThreadPool() { stop(); }

    void start();
    void stop();
    size_t size() const { return workers_.size(); }

    // 提交任务，返回 future 以便异步获取结果
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using R = typename std::invoke_result<F, Args...>::type;
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<R> fut = task->get_future();
        enqueue([task]() { (*task)(); });
        return fut;
    }

    // 提交不关心结果的普通任务
    void enqueue(Task t);

private:
    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace transcode
