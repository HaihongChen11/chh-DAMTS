#pragma once

#include <vector>
#include <sys/epoll.h>

#include "common/noncopyable.h"

namespace transcode {

// epoll 系统调用封装。
// 统一使用 ET（边缘触发）+ EPOLLRDHUP（对端半关闭检测）。
class Epoller : Noncopyable {
public:
    using EventList = std::vector<struct epoll_event>;

    Epoller();
    ~Epoller();

    void addFd(int fd, uint32_t events);
    void modFd(int fd, uint32_t events);
    void delFd(int fd);

    // 等待事件，返回活跃 fd 数量；返回 0 表示超时；返回 -1 表示出错
    int poll(int timeout_ms, EventList& active_events);

private:
    int epollfd_;
    EventList events_;
};

} // namespace transcode
