#include "reactor/event_loop.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <cerrno>

#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "common/logger.h"
#include "reactor/tcp_connection.h"

namespace transcode {

namespace {
int createEventfd() {
    int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("eventfd create failed: {}", strerror(errno));
        std::abort();
    }
    return fd;
}

int createTimerfd() {
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("timerfd create failed: {}", strerror(errno));
        std::abort();
    }
    return fd;
}
} // namespace

EventLoop::EventLoop()
    : wakeupFd_(createEventfd()),
      timerfd_(createTimerfd()),
      threadId_(std::this_thread::get_id()) {
    poller_.addFd(wakeupFd_, EPOLLIN);
    poller_.addFd(timerfd_, EPOLLIN);
}

EventLoop::~EventLoop() {
    ::close(wakeupFd_);
    ::close(timerfd_);
}

int64_t EventLoop::nowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void EventLoop::loop() {
    assertInLoopThread();
    looping_ = true;
    quit_ = false;

    Epoller::EventList activeEvents;
    while (!quit_) {
        activeEvents.clear();
        // 阻塞等待事件；timerfd/eventfd 会负责唤醒
        int n = poller_.poll(-1, activeEvents);
        if (n < 0) continue;

        for (const auto& ev : activeEvents) {
            int fd = ev.data.fd;
            if (fd == wakeupFd_) {
                handleWakeup();
                continue;
            }
            if (fd == timerfd_) {
                handleTimerfd();
                continue;
            }

            auto eit = externalFds_.find(fd);
            if (eit != externalFds_.end()) {
                eit->second(ev.events);
                continue;
            }

            auto it = connections_.find(fd);
            if (it == connections_.end()) continue;
            std::shared_ptr<TcpConnection> conn = it->second; // 延长生命周期

            uint32_t events = ev.events;
            // 先处理读写，再处理关闭/错误。客户端可能同时带来 EPOLLIN 与
            // EPOLLRDHUP/EPOLLHUP，若先关闭会丢失已到达的请求数据。
            if (events & (EPOLLIN | EPOLLRDHUP)) conn->handleRead();
            if (events & EPOLLOUT) conn->handleWrite();
            if (events & EPOLLERR) conn->handleError();
            if (events & EPOLLHUP) conn->handleClose();
        }
        doPendingFunctors();
    }
    looping_ = false;
}

void EventLoop::quit() {
    quit_ = true;
    if (!isInLoopThread()) wakeup();
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }
    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}

void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    (void)n;
}

void EventLoop::handleWakeup() {
    uint64_t one = 0;
    ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
    (void)n;
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        functors.swap(pendingFunctors_);
    }
    for (auto& f : functors) f();
    callingPendingFunctors_ = false;
}

// ---------------- 连接管理 ----------------

void EventLoop::addConnection(const std::shared_ptr<TcpConnection>& conn) {
    assertInLoopThread();
    connections_[conn->fd()] = conn;
    poller_.addFd(conn->fd(), conn->events());
}

void EventLoop::removeConnection(int fd) {
    assertInLoopThread();
    poller_.delFd(fd);
    connections_.erase(fd);
}

void EventLoop::updateEvents(int fd, uint32_t events) {
    assertInLoopThread();
    poller_.modFd(fd, events);
}

void EventLoop::forEachConnection(
    const std::function<void(const std::shared_ptr<TcpConnection>&)>& fn) {
    assertInLoopThread();
    std::vector<std::shared_ptr<TcpConnection>> conns;
    conns.reserve(connections_.size());
    for (auto& kv : connections_) conns.push_back(kv.second);
    for (auto& c : conns) fn(c);
}

void EventLoop::registerExternalFd(int fd, uint32_t events, std::function<void(uint32_t)> cb) {
    assertInLoopThread();
    poller_.addFd(fd, events);
    externalFds_[fd] = std::move(cb);
}

void EventLoop::unregisterExternalFd(int fd) {
    assertInLoopThread();
    poller_.delFd(fd);
    externalFds_.erase(fd);
}

// ---------------- 定时器 ----------------

EventLoop::TimerId EventLoop::runAfter(int64_t delay_ms, TimerCallback cb) {
    assertInLoopThread();
    Timer t;
    t.id = nextTimerId_++;
    t.expiration = nowMs() + delay_ms;
    t.interval = 0;
    t.cb = std::move(cb);

    timers_.emplace(t.expiration, t);
    timerIndex_[t.id] = t.expiration;
    resetTimerfd(timers_.begin()->first);
    return t.id;
}

EventLoop::TimerId EventLoop::runEvery(int64_t interval_ms, TimerCallback cb) {
    assertInLoopThread();
    Timer t;
    t.id = nextTimerId_++;
    t.expiration = nowMs() + interval_ms;
    t.interval = interval_ms;
    t.cb = std::move(cb);

    timers_.emplace(t.expiration, t);
    timerIndex_[t.id] = t.expiration;

    // 用最早到期时间重置 timerfd
    resetTimerfd(timers_.begin()->first);
    return t.id;
}

void EventLoop::cancel(TimerId id) {
    assertInLoopThread();
    auto it = timerIndex_.find(id);
    if (it != timerIndex_.end()) {
        cancelledTimers_.insert(id);
        timerIndex_.erase(it);
    }
}

void EventLoop::resetTimerfd(int64_t earliest_ms) {
    struct itimerspec ts;
    std::memset(&ts, 0, sizeof(ts));
    int64_t delay = earliest_ms - nowMs();
    if (delay < 0) delay = 0;
    ts.it_value.tv_sec = delay / 1000;
    ts.it_value.tv_nsec = (delay % 1000) * 1000000;
    ::timerfd_settime(timerfd_, 0, &ts, nullptr);
}

void EventLoop::handleTimerfd() {
    uint64_t expirations = 0;
    ssize_t n = ::read(timerfd_, &expirations, sizeof(expirations));
    (void)n;

    int64_t now = nowMs();
    std::vector<Timer> ready;

    // 取出所有已到期定时器
    auto it = timers_.begin();
    while (it != timers_.end() && it->first <= now) {
        const Timer& t = it->second;
        if (cancelledTimers_.count(t.id) == 0) {
            ready.push_back(t);
        }
        timerIndex_.erase(t.id);
        it = timers_.erase(it);
    }

    for (auto& t : ready) {
        t.cb();
        // 周期定时器重新入队
        if (t.interval > 0 && cancelledTimers_.count(t.id) == 0) {
            Timer nt = t;
            nt.expiration = now + t.interval;
            timers_.emplace(nt.expiration, nt);
            timerIndex_[nt.id] = nt.expiration;
        }
    }
    for (uint64_t id : cancelledTimers_) cancelledTimers_.erase(id);

    if (!timers_.empty()) resetTimerfd(timers_.begin()->first);
}

void EventLoop::assertInLoopThread() {
    if (!isInLoopThread()) {
        LOG_ERROR("EventLoop::assertInLoopThread failed, not in loop thread");
        std::abort();
    }
}

} // namespace transcode
