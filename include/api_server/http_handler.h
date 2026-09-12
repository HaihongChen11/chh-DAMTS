#pragma once

#include "reactor/http_server.h"
#include "reactor/router.h"
#include "service/service_registry.h"

namespace transcode {

// 注册所有 HTTP 路由（handler 内完成业务并填充响应）
void registerRoutes(Router* router, ServiceRegistry* svc);

// 构造 HttpServer 的回调：把解析完成的请求投递到业务线程池后分发。
// 保证 epoll 主循环不执行任何阻塞 / CPU 密集业务。
HttpServer::HttpCallback makeHttpCallback(Router* router, ServiceRegistry* svc);

} // namespace transcode
