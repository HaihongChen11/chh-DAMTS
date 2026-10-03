#include "reactor/http_server.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/logger.h"
#include "reactor/event_loop.h"
#include "reactor/http_response.h"

namespace transcode {

HttpServer::HttpServer(EventLoop* acceptLoop, const std::vector<EventLoop*>& subLoops,
                       const std::string& ip, uint16_t port)
    : loop_(acceptLoop), subLoops_(subLoops), ip_(ip), port_(port) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (listenFd_ < 0) {
        LOG_ERROR("socket create failed: {}", strerror(errno));
        std::abort();
    }

    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1) {
        LOG_ERROR("invalid listen ip: {}", ip_);
        std::abort();
    }

    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("bind {}:{} failed: {}", ip_, port_, strerror(errno));
        std::abort();
    }
    if (::listen(listenFd_, SOMAXCONN) < 0) {
        LOG_ERROR("listen failed: {}", strerror(errno));
        std::abort();
    }

    // 监听 fd 注册进事件循环（accept 事件回调）
    loop_->registerExternalFd(listenFd_, EPOLLIN,
                              [this](uint32_t events) { onAccept(events); });
}

HttpServer::~HttpServer() {
    if (listenFd_ >= 0) ::close(listenFd_);
}

void HttpServer::start() {
    // 定时扫描空闲连接，回收超时 fd，防止 fd 泄漏
    loop_->runEvery(1000, [this]() {
        int64_t now = TcpConnection::steadyNowMs();
        loop_->forEachConnection([now, this](const TcpConnectionPtr& conn) {
            if (conn->connected() && now - conn->lastActiveTime() > connectionTimeoutMs_) {
                LOG_INFO("close idle connection fd={}", conn->fd());
                conn->forceClose();
            }
        });
    });
    LOG_INFO("http server started on {}:{}", ip_, port_);
}

void HttpServer::onAccept(uint32_t /*events*/) {
    for (;;) {
        if (!accepting_.load()) break;
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int connfd = ::accept4(listenFd_, reinterpret_cast<struct sockaddr*>(&peer), &len,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (connfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            LOG_ERROR("accept failed: {}", strerror(errno));
            break;
        }

        EventLoop* connLoop = subLoops_[nextLoopIndex_ % subLoops_.size()];
        ++nextLoopIndex_;
        auto conn = std::make_shared<TcpConnection>(connLoop, connfd);
        conn->setConnectionCallback([this](const TcpConnectionPtr& c) { onConnection(c); });
        conn->setMessageCallback([this](const TcpConnectionPtr& c, Buffer* b) { onMessage(c, b); });
        conn->setCloseCallback([this](const TcpConnectionPtr& c) { onClose(c); });
        conn->connectEstablished();
    }
}

void HttpServer::onConnection(const TcpConnectionPtr& conn) {
    // HttpContext 内含不可拷贝的 HttpParser，std::any 无法直接按值存储。
    // 用 shared_ptr 包装后存入 std::any，生命周期由连接上下文持有。
    conn->setContext(std::make_shared<HttpContext>());
}

void HttpServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
    auto* ctxPtr = std::any_cast<std::shared_ptr<HttpContext>>(conn->getMutableContext());
    if (ctxPtr == nullptr || *ctxPtr == nullptr || (*ctxPtr)->busy) return;
    auto& ctx = **ctxPtr;

    ParseResult r = ctx.parser.parse(buf);
    if (r == ParseResult::kNeedMore) return;

    if (r == ParseResult::kError) {
        LOG_WARN("http parse error: {}", ctx.parser.error());
        HttpResponse resp = HttpResponse::badRequest(ctx.parser.error());
        conn->send(resp.toString());
        conn->shutdown();
        return;
    }

    // 解析完成，交给业务层
    HttpRequest req = std::move(ctx.parser.request());
    ctx.parser.reset();
    ctx.busy = true;
    if (httpCallback_) httpCallback_(std::move(req), conn);
}

void HttpServer::onResponseSent(const TcpConnectionPtr& conn) {
    conn->getLoop()->runInLoop([conn]() {
        auto* ctxPtr = std::any_cast<std::shared_ptr<HttpContext>>(
            conn->getMutableContext());
        if (ctxPtr && *ctxPtr) {
            (*ctxPtr)->busy = false;
        }
    });
}

void HttpServer::onClose(const TcpConnectionPtr& /*conn*/) {
    // 连接已从事件循环移除，对象随后析构回收 fd
}

} // namespace transcode
