#include "reactor/tcp_connection.h"

#include <chrono>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/logger.h"
#include "reactor/event_loop.h"

namespace transcode {

TcpConnection::TcpConnection(EventLoop* loop, int sockfd)
    : loop_(loop), fd_(sockfd), events_(EPOLLIN) {}

TcpConnection::~TcpConnection() {
    if (fd_ >= 0) ::close(fd_);
}

int64_t TcpConnection::steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------- socket 工具 ----------------

int TcpConnection::createNonblockingOrDie() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        LOG_ERROR("socket create failed: {}", strerror(errno));
        std::abort();
    }
    return fd;
}

void TcpConnection::setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void TcpConnection::setTcpNoDelay(int fd, bool on) {
    int opt = on ? 1 : 0;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

// ---------------- 生命周期 ----------------

void TcpConnection::connectEstablished() {
    setState(kConnected);
    setTcpNoDelay(fd_, true);
    events_ = EPOLLIN;
    lastActiveTime_ = steadyNowMs();
    loop_->runInLoop([conn = shared_from_this()]() {
        conn->loop_->addConnection(conn);
        if (conn->connectionCallback_) conn->connectionCallback_(conn);
    });
}

void TcpConnection::connectDestroyed() {
    if (state_ == kConnected) setState(kDisconnected);
}

// ---------------- 发送 ----------------

void TcpConnection::send(const std::string& msg) {
    if (state_ == kConnected) {
        loop_->runInLoop([conn = shared_from_this(), msg]() { conn->sendInLoop(msg); });
    }
}

void TcpConnection::send(Buffer* buf) {
    if (state_ == kConnected) {
        loop_->runInLoop([conn = shared_from_this(), buf]() { conn->sendInLoop(buf); });
    }
}

void TcpConnection::sendInLoop(const std::string& msg) {
    if (state_ != kConnected) return;

    ssize_t n = 0;
    size_t remaining = msg.size();
    // 发送缓冲区为空时先尝试直接 write，减少一次拷贝
    if (outputBuffer_.readableBytes() == 0) {
        n = ::write(fd_, msg.data(), msg.size());
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                LOG_ERROR("write error fd={} errno={}", fd_, errno);
                handleError();
                return;
            }
            n = 0;
        } else {
            remaining -= static_cast<size_t>(n);
        }
    }
    if (remaining > 0) {
        outputBuffer_.append(msg.data() + n, remaining);
        if (!writing_) {
            writing_ = true;
            events_ |= EPOLLOUT;
            loop_->updateEvents(fd_, events_);
        }
    }
}

void TcpConnection::sendInLoop(Buffer* buf) {
    if (state_ != kConnected) return;
    if (outputBuffer_.readableBytes() == 0) {
        outputBuffer_.swap(*buf);
    } else {
        outputBuffer_.append(buf->peek(), buf->readableBytes());
    }
    if (!writing_) {
        writing_ = true;
        events_ |= EPOLLOUT;
        loop_->updateEvents(fd_, events_);
    }
}

void TcpConnection::shutdown() {
    if (state_ == kConnected) {
        loop_->runInLoop([conn = shared_from_this()]() { conn->shutdownInLoop(); });
    }
}

void TcpConnection::shutdownInLoop() {
    if (state_ == kDisconnected) return;
    if (state_ == kConnected) setState(kDisconnecting);
    if (!writing_) {
        ::shutdown(fd_, SHUT_WR);
    }
}

void TcpConnection::forceClose() {
    if (state_ == kConnected || state_ == kDisconnecting) {
        setState(kDisconnecting);
        loop_->runInLoop([conn = shared_from_this()]() { conn->forceCloseInLoop(); });
    }
}

void TcpConnection::forceCloseInLoop() {
    if (state_ == kDisconnected) return;
    setState(kDisconnecting);
    handleClose();
}

// ---------------- 事件处理（仅 IO 线程） ----------------

void TcpConnection::handleRead() {
    lastActiveTime_ = steadyNowMs();
    int savedErrno = 0;
    ssize_t n = 0;
    for (;;) {
        n = inputBuffer_.readFd(fd_, &savedErrno);
        if (n > 0) {
            // 每读到一批数据立即交给上层（HTTP 解析器流式消费）
            if (messageCallback_) messageCallback_(shared_from_this(), &inputBuffer_);
            continue;
        } else if (n == 0) {
            handleClose(); // 对端关闭
            return;
        } else {
            if (savedErrno == EINTR) continue;
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) return; // 已读完
            LOG_ERROR("read error fd={} errno={}", fd_, savedErrno);
            handleError();
            return;
        }
    }
}

void TcpConnection::handleWrite() {
    lastActiveTime_ = steadyNowMs();
    int savedErrno = 0;
    ssize_t n = outputBuffer_.writeFd(fd_, &savedErrno);
    if (n < 0 && savedErrno != EAGAIN && savedErrno != EWOULDBLOCK) {
        LOG_ERROR("write error fd={} errno={}", fd_, savedErrno);
        handleError();
        return;
    }
    if (outputBuffer_.readableBytes() == 0) {
        writing_ = false;
        events_ &= ~EPOLLOUT;
        loop_->updateEvents(fd_, events_);
        if (writeCompleteCallback_) writeCompleteCallback_(shared_from_this());
        if (state_ == kDisconnecting) shutdownInLoop();
    }
}

void TcpConnection::handleClose() {
    if (state_ == kDisconnected) return;
    setState(kDisconnected);
    loop_->removeConnection(fd_);
    if (closeCallback_) closeCallback_(shared_from_this());
}

void TcpConnection::handleError() {
    LOG_WARN("connection error fd={} errno={}", fd_, errno);
    handleClose();
}

} // namespace transcode
