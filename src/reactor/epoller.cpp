#include "reactor/epoller.h"

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>

#include "common/logger.h"

namespace transcode {

namespace {
constexpr uint32_t kBaseEvents = EPOLLET | EPOLLRDHUP;
}

Epoller::Epoller() : events_(1024) {
    epollfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollfd_ < 0) {
        LOG_ERROR("epoll_create1 failed: {}", strerror(errno));
        std::abort();
    }
}

Epoller::~Epoller() {
    if (epollfd_ >= 0) ::close(epollfd_);
}

void Epoller::addFd(int fd, uint32_t events) {
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = events | kBaseEvents;
    ev.data.fd = fd;
    ::epoll_ctl(epollfd_, EPOLL_CTL_ADD, fd, &ev);
}

void Epoller::modFd(int fd, uint32_t events) {
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = events | kBaseEvents;
    ev.data.fd = fd;
    ::epoll_ctl(epollfd_, EPOLL_CTL_MOD, fd, &ev);
}

void Epoller::delFd(int fd) {
    ::epoll_ctl(epollfd_, EPOLL_CTL_DEL, fd, nullptr);
}

int Epoller::poll(int timeout_ms, EventList& active_events) {
    int n = ::epoll_wait(epollfd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return 0;
        LOG_ERROR("epoll_wait failed: {}", strerror(errno));
        return -1;
    }
    active_events.assign(events_.begin(), events_.begin() + n);
    // 事件容器用满则翻倍扩容，避免事件丢失
    if (n == static_cast<int>(events_.size())) {
        events_.resize(events_.size() * 2);
    }
    return n;
}

} // namespace transcode
