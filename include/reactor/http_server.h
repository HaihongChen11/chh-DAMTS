#pragma once

#include <any>
#include <functional>
#include <memory>
#include <string>

#include "common/noncopyable.h"
#include "reactor/http_parser.h"
#include "reactor/tcp_connection.h"

namespace transcode {

class EventLoop;

// 每连接 HTTP 上下文（挂在 TcpConnection 的 context 上）
struct HttpContext {
    HttpParser parser;
    bool busy = false; // true 表示请求已提交线程池、等待响应，不再接收新请求
};

// HTTP 服务：监听 + accept + 解析 + 分发。
// 解析完成后把完整请求交给 HttpCallback（业务层），业务层决定同步应答还是
// 投递线程池异步应答；事件循环本身绝不执行阻塞业务。
class HttpServer : Noncopyable {
public:
    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
    using HttpCallback = std::function<void(HttpRequest, TcpConnectionPtr)>;

    HttpServer(EventLoop* loop, const std::string& ip, uint16_t port);
    ~HttpServer();

    void setHttpCallback(HttpCallback cb) { httpCallback_ = std::move(cb); }
    void setConnectionTimeoutMs(int64_t ms) { connectionTimeoutMs_ = ms; }

    void start();

private:
    void onAccept(uint32_t events);
    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf);
    void onClose(const TcpConnectionPtr& conn);

    EventLoop* loop_;
    std::string ip_;
    uint16_t port_;
    int listenFd_ = -1;
    int64_t connectionTimeoutMs_ = 60000;
    HttpCallback httpCallback_;
};

} // namespace transcode
