#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "common/noncopyable.h"
#include "reactor/buffer.h"

namespace transcode {

class EventLoop;

// TCP 连接：封装 fd、读写缓冲区、连接状态机与回调。
// 生命周期由 shared_ptr 管理，事件循环持有引用，业务层通过 shared_from_this 延长。
//
// 连接状态机：
//   kConnecting -> kConnected -> kDisconnecting -> kDisconnected
class TcpConnection : public std::enable_shared_from_this<TcpConnection>, Noncopyable {
public:
    enum StateE { kConnecting, kConnected, kDisconnecting, kDisconnected };

    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
    using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
    using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;

    TcpConnection(EventLoop* loop, int sockfd);
    ~TcpConnection();

    EventLoop* getLoop() const { return loop_; }
    int fd() const { return fd_; }
    uint32_t events() const { return events_; }
    StateE state() const { return state_; }
    int64_t lastActiveTime() const { return lastActiveTime_; }
    bool connected() const { return state_ == kConnected; }

    static int64_t steadyNowMs();

    Buffer* inputBuffer() { return &inputBuffer_; }
    Buffer* outputBuffer() { return &outputBuffer_; }

    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

    // 每连接业务上下文（HTTP 解析状态等）
    void setContext(std::any ctx) { context_ = std::move(ctx); }
    std::any* getMutableContext() { return &context_; }

    // accept 后调用：加入事件循环并注册可读
    void connectEstablished();
    // 连接销毁后调用：兜底关闭 fd
    void connectDestroyed();

    // 发送数据（线程安全，跨线程投递回 IO 线程执行）
    void send(const std::string& msg);
    void send(Buffer* buf);

    // 半关闭写端
    void shutdown();
    void shutdownInLoop();
    // 立即关闭连接
    void forceClose();
    void forceCloseInLoop();

    // 事件循环回调（仅 IO 线程调用）
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    // 静态 socket 工具
    static int createNonblockingOrDie();
    static void setNonBlocking(int fd);
    static void setTcpNoDelay(int fd, bool on);

private:
    void sendInLoop(const std::string& msg);
    void sendInLoop(Buffer* buf);
    void setState(StateE s) { state_ = s; }

    EventLoop* loop_;
    const int fd_;
    StateE state_ = kConnecting;

    uint32_t events_ = 0; // 关注的事件（不含 EPOLLET/RDHUP，由 Epoller 补充）
    bool writing_ = false;
    int64_t lastActiveTime_ = 0;

    Buffer inputBuffer_;
    Buffer outputBuffer_;

    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    ConnectionCallback connectionCallback_;
    WriteCompleteCallback writeCompleteCallback_;

    std::any context_;
};

} // namespace transcode
