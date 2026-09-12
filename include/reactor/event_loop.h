#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/noncopyable.h"
#include "reactor/epoller.h"

namespace transcode {

class TcpConnection;

// 定时器描述
struct Timer {
    uint64_t id;
    int64_t expiration; // 绝对过期时刻（steady_clock，毫秒）
    int64_t interval;   // 0 表示一次性；>0 表示周期
    std::function<void()> cb;
};

// 手写 epoll Reactor 事件循环（单线程模型，类似 muduo 的 EventLoop）。
// 职责：IO 事件分发 + 定时器调度；绝不允许在循环内执行阻塞 / CPU 密集业务。
// 跨线程任务通过 eventfd 唤醒后由本线程执行（doPendingFunctors）。
class EventLoop : Noncopyable {
public:
    using Functor = std::function<void()>;
    using TimerCallback = std::function<void()>;
    using TimerId = uint64_t;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit();

    // 线程安全：将任务投递回 IO 线程执行（供业务线程池回调使用）
    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);

    // 连接管理（仅 IO 线程内调用）
    void addConnection(const std::shared_ptr<TcpConnection>& conn);
    void removeConnection(int fd);
    void updateEvents(int fd, uint32_t events);
    // 遍历所有连接（用于超时扫描），内部先拷贝快照，可在回调中安全关闭连接
    void forEachConnection(const std::function<void(const std::shared_ptr<TcpConnection>&)>& fn);

    // 通用 fd 注册（监听 socket、metrics 等），回调在 IO 线程内触发
    void registerExternalFd(int fd, uint32_t events, std::function<void(uint32_t)> cb);
    void unregisterExternalFd(int fd);

    // 定时器（仅 IO 线程内调用）
    TimerId runAfter(int64_t delay_ms, TimerCallback cb);
    TimerId runEvery(int64_t interval_ms, TimerCallback cb);
    void cancel(TimerId id);

    bool isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }
    void assertInLoopThread();

private:
    void wakeup();
    void handleWakeup();
    void handleTimerfd();
    void doPendingFunctors();
    int64_t nowMs() const;
    void resetTimerfd(int64_t earliest_ms);

    Epoller poller_;
    std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_;
    std::unordered_map<int, std::function<void(uint32_t)>> externalFds_;

    int wakeupFd_;  // eventfd，用于跨线程唤醒
    int timerfd_;   // timerfd，用于定时器到期唤醒

    std::mutex mutex_;
    std::vector<Functor> pendingFunctors_;
    bool callingPendingFunctors_ = false;

    std::atomic<bool> looping_{false};
    std::atomic<bool> quit_{false};
    std::thread::id threadId_;

    // 定时器
    std::multimap<int64_t, Timer> timers_;              // 按过期时刻有序
    std::unordered_map<uint64_t, int64_t> timerIndex_;  // id -> expiration
    std::set<uint64_t> cancelledTimers_;
    uint64_t nextTimerId_ = 1;
};

} // namespace transcode
