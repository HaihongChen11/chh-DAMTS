#pragma once

#include <functional>
#include <map>
#include <string>

#include "reactor/http_parser.h"
#include "reactor/http_response.h"

namespace transcode {

// HTTP 路由：根据 method + path 分发到对应业务处理函数。
// 业务处理函数只负责填充响应，阻塞操作由业务线程池完成（在 handler 内投递）。
class Router {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse*)>;

    void addRoute(const std::string& method, const std::string& path, Handler h);

    // 命中返回 true 并调用 handler；未命中返回 false 并写入 404
    bool route(const HttpRequest& req, HttpResponse* resp) const;

private:
    static std::string key(const std::string& method, const std::string& path);

    std::map<std::string, Handler> routes_;
};

} // namespace transcode
