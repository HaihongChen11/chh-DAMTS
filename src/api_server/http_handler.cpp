#include "api_server/http_handler.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "common/logger.h"
#include "metrics/metrics.h"
#include "reactor/http_response.h"
#include "service/task_service.h"

namespace transcode {

namespace {

// 返回浏览器可访问的 Web 控制台页面（单文件前端）
void serveWebConsole(HttpResponse* resp) {
    std::ifstream ifs("web/index.html", std::ios::binary);
    if (!ifs.is_open()) {
        *resp = HttpResponse::internalError("web/index.html not found");
        return;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    resp->setContentType("text/html; charset=utf-8");
    resp->setBody(ss.str());
}

// 从 Authorization: Bearer <token> 提取 token
std::string bearerToken(const HttpRequest& req) {
    std::string auth = req.getHeader("authorization");
    if (auth.rfind("Bearer ", 0) == 0) return auth.substr(7);
    return auth;
}

// 鉴权：返回 user_id，无效时填充 401 响应并返回 0
int64_t requireAuth(ServiceRegistry* svc, const HttpRequest& req, HttpResponse* resp) {
    int64_t uid = svc->auth->authenticate(bearerToken(req));
    if (uid == 0) *resp = HttpResponse::unauthorized("invalid or missing token");
    return uid;
}

nlohmann::json parseJsonBody(const HttpRequest& req, HttpResponse* resp) {
    try {
        return nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
        *resp = HttpResponse::badRequest("invalid json body");
        return nlohmann::json();
    }
}

// ---------------- 各接口 handler ----------------

void handleRegister(ServiceRegistry* svc, const HttpRequest& req, HttpResponse* resp) {
    nlohmann::json body = parseJsonBody(req, resp);
    if (body.is_null()) return;
    std::string err;
    if (svc->auth->registerUser(body.value("username", ""), body.value("password", ""), &err)) {
        *resp = HttpResponse::created(nlohmann::json{{"message", "ok"}}.dump());
    } else {
        *resp = HttpResponse::badRequest(err.empty() ? "register failed" : err);
    }
}

void handleLogin(ServiceRegistry* svc, const HttpRequest& req, HttpResponse* resp) {
    nlohmann::json body = parseJsonBody(req, resp);
    if (body.is_null()) return;
    std::string token = svc->auth->login(body.value("username", ""), body.value("password", ""));
    if (token.empty()) {
        *resp = HttpResponse::unauthorized("invalid username or password");
    } else {
        *resp = HttpResponse::ok(nlohmann::json{{"token", token}}.dump());
    }
}

void handleLogout(ServiceRegistry* svc, const HttpRequest& req, HttpResponse* resp) {
    svc->auth->logout(bearerToken(req));
    *resp = HttpResponse::ok(nlohmann::json{{"message", "ok"}}.dump());
}

void handleSubmitTask(ServiceRegistry* svc, const HttpRequest& req, HttpResponse* resp) {
    int64_t uid = requireAuth(svc, req, resp);
    if (uid == 0) return;

    // 滑动窗口限流（按用户）
    if (!svc->rateLimiter->allow("rate:" + std::to_string(uid))) {
        *resp = HttpResponse::tooManyRequests("rate limit exceeded");
        return;
    }

    if (!req.has_file) {
        *resp = HttpResponse::badRequest("missing video file");
        return;
    }

    const auto& fields = req.form_fields;
    auto get = [&fields](const std::string& k, const std::string& def) {
        auto it = fields.find(k);
        return it == fields.end() ? def : it->second;
    };
    std::string resolution = get("resolution", "1280x720");
    int bitrate = svc->cfg.worker().default_bitrate;
    try {
        bitrate = std::stoi(get("bitrate", std::to_string(svc->cfg.worker().default_bitrate)));
    } catch (...) {
        bitrate = svc->cfg.worker().default_bitrate;
    }
    if (bitrate < 100000) bitrate = svc->cfg.worker().default_bitrate;
    std::string videoName = get("video_name", req.file.filename);
    std::string tag = get("tag", "");
    std::string idemKey = req.getHeader("idempotency-key");

    auto result = svc->task->submitTask(uid, videoName, tag, resolution, bitrate, req.file, idemKey);
    if (result.ok) {
        Metrics::instance().incTaskSubmitted();
        *resp = HttpResponse::created(nlohmann::json{{"task_id", result.task_id}}.dump());
    } else {
        *resp = HttpResponse::internalError(result.error);
    }
}

void handleQueryTask(ServiceRegistry* svc, const HttpRequest& req, HttpResponse* resp) {
    int64_t uid = requireAuth(svc, req, resp);
    if (uid == 0) return;
    auto params = req.queryParams();
    std::string taskId = params.count("task_id") ? params["task_id"] : "";
    if (taskId.empty()) {
        *resp = HttpResponse::badRequest("missing task_id");
        return;
    }
    nlohmann::json j = svc->task->getTaskStatus(uid, taskId);
    if (j.contains("error") && j["error"] == "not found") {
        *resp = HttpResponse::notFound("task not found");
        return;
    }
    if (j.contains("error") && j["error"] == "forbidden") {
        *resp = HttpResponse::forbidden("access denied");
        return;
    }
    *resp = HttpResponse::ok(j.dump());
}

void handlePresign(ServiceRegistry* svc, const HttpRequest& req, HttpResponse* resp) {
    int64_t uid = requireAuth(svc, req, resp);
    if (uid == 0) return;
    auto params = req.queryParams();
    std::string taskId = params.count("task_id") ? params["task_id"] : "";
    std::string url = taskId.empty() ? "" : svc->task->getPresignedUrl(uid, taskId);
    if (url.empty()) {
        *resp = HttpResponse::badRequest("task not ready or not found");
        return;
    }
    *resp = HttpResponse::ok(nlohmann::json{{"url", url}}.dump());
}

void handleSearch(ServiceRegistry* svc, const HttpRequest& req, HttpResponse* resp) {
    int64_t uid = requireAuth(svc, req, resp);
    if (uid == 0) return;
    auto params = req.queryParams();
    std::string q = params.count("q") ? params["q"] : "";
    std::string result = svc->task->search(uid, q);
    *resp = HttpResponse::ok(result);
}

} // namespace

void registerRoutes(Router* router, ServiceRegistry* svc) {
    router->addRoute("GET", "/", [](const HttpRequest&, HttpResponse* resp) {
        serveWebConsole(resp);
    });
    router->addRoute("GET", "/index.html", [](const HttpRequest&, HttpResponse* resp) {
        serveWebConsole(resp);
    });
    router->addRoute("GET", "/healthz", [](const HttpRequest&, HttpResponse* resp) {
        *resp = HttpResponse::ok(nlohmann::json{{"status", "ok"}}.dump());
    });
    router->addRoute("GET", "/metrics", [](const HttpRequest&, HttpResponse* resp) {
        HttpResponse r = HttpResponse::ok(Metrics::instance().serialize());
        r.setContentType("text/plain; version=0.0.4; charset=utf-8");
        *resp = r;
    });
    router->addRoute("POST", "/api/register", [svc](const HttpRequest& req, HttpResponse* resp) {
        handleRegister(svc, req, resp);
    });
    router->addRoute("POST", "/api/login", [svc](const HttpRequest& req, HttpResponse* resp) {
        handleLogin(svc, req, resp);
    });
    router->addRoute("POST", "/api/logout", [svc](const HttpRequest& req, HttpResponse* resp) {
        handleLogout(svc, req, resp);
    });
    router->addRoute("POST", "/api/tasks", [svc](const HttpRequest& req, HttpResponse* resp) {
        handleSubmitTask(svc, req, resp);
    });
    router->addRoute("GET", "/api/tasks", [svc](const HttpRequest& req, HttpResponse* resp) {
        handleQueryTask(svc, req, resp);
    });
    router->addRoute("GET", "/api/tasks/presign", [svc](const HttpRequest& req, HttpResponse* resp) {
        handlePresign(svc, req, resp);
    });
    router->addRoute("GET", "/api/search", [svc](const HttpRequest& req, HttpResponse* resp) {
        handleSearch(svc, req, resp);
    });
}

HttpServer::HttpCallback makeHttpCallback(Router* router, ServiceRegistry* svc) {
    return [router, svc](HttpRequest req, HttpServer::TcpConnectionPtr conn) {
        // 立即投递业务线程池，epoll 主循环马上回归继续处理其它连接
        svc->threadPool->enqueue([router, svc, req = std::move(req), conn = std::move(conn)]() {
            Metrics::instance().incQps();
            HttpResponse resp;
            router->route(req, &resp);
            conn->send(resp.toString());
            conn->shutdown();
        });
    };
}

} // namespace transcode
