#include "api_server/http_handler.h"

#include <fstream>
#include <atomic>
#include <sstream>

#include <nlohmann/json.hpp>
#include <sw/redis++/redis.h>

#include "common/logger.h"
#include "common/uuid.h"
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

    auto result = svc->task->submitTask(uid, videoName, tag, resolution, bitrate,
                                        req.file, idemKey, req.getHeader("x-trace-id"));
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

void handlePresignUpload(ServiceRegistry* svc, const HttpRequest& req, HttpResponse* resp) {
    int64_t uid = requireAuth(svc, req, resp);
    if (uid == 0) return;
    auto params = req.queryParams();
    std::string filename = params.count("filename") ? params["filename"] : "video.mp4";
    std::string key = "users/" + std::to_string(uid) + "/" + generateUuid() + "/" + filename;
    std::string url = svc->storage->presignUploadUrl(svc->cfg.minio().source_bucket, key, 3600);
    *resp = HttpResponse::ok(nlohmann::json{{"upload_url", url}, {"source_key", key}}.dump());
}

void handleDirectTask(ServiceRegistry* svc, const HttpRequest& req, HttpResponse* resp) {
    int64_t uid = requireAuth(svc, req, resp);
    if (uid == 0) return;
    if (!svc->rateLimiter->allow("rate:" + std::to_string(uid))) {
        *resp = HttpResponse::tooManyRequests("rate limit exceeded");
        return;
    }
    nlohmann::json body = parseJsonBody(req, resp);
    if (body.is_null()) return;
    std::string sourceKey = body.value("source_key", "");
    if (sourceKey.empty()) {
        *resp = HttpResponse::badRequest("missing source_key");
        return;
    }
    std::string resolution = body.value("resolution", "1280x720");
    int bitrate = svc->cfg.worker().default_bitrate;
    try { bitrate = std::stoi(body.value("bitrate", std::to_string(bitrate))); } catch (...) {}
    if (bitrate < 100000) bitrate = svc->cfg.worker().default_bitrate;
    auto result = svc->task->submitTaskFromSource(
        uid, body.value("video_name", "video"), body.value("tag", ""),
        resolution, bitrate, sourceKey, req.getHeader("idempotency-key"),
        req.getHeader("x-trace-id"));
    if (result.ok) {
        Metrics::instance().incTaskSubmitted();
        *resp = HttpResponse::created(nlohmann::json{{"task_id", result.task_id}}.dump());
    } else {
        *resp = HttpResponse::internalError(result.error);
    }
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
    router->addRoute("GET", "/readyz", [svc](const HttpRequest&, HttpResponse* resp) {
        nlohmann::json j;
        j["status"] = "ok";

        try {
            svc->redis->ping();
            j["redis"] = "ok";
        } catch (const std::exception&) {
            j["redis"] = "down";
            j["status"] = "degraded";
        }

        j["mysql"] = svc->mysql->ping() ? "ok" : "down";
        if (j["mysql"] == "down") j["status"] = "degraded";

        j["rabbitmq"] = svc->mq->isConnected() ? "ok" : "down";
        if (j["rabbitmq"] == "down") j["status"] = "degraded";

        *resp = HttpResponse::ok(j.dump());
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
    router->addRoute("GET", "/api/presign-upload", [svc](const HttpRequest& req, HttpResponse* resp) {
        handlePresignUpload(svc, req, resp);
    });
    router->addRoute("POST", "/api/tasks/direct", [svc](const HttpRequest& req, HttpResponse* resp) {
        handleDirectTask(svc, req, resp);
    });
}

HttpServer::HttpCallback makeHttpCallback(Router* router, ServiceRegistry* svc,
                                          HttpServer* server) {
    return [router, svc, server](HttpRequest req, HttpServer::TcpConnectionPtr conn) {
        const bool keepAlive = (req.getHeader("connection") != "close");
        std::string traceId = req.getHeader("x-trace-id");
        if (traceId.empty()) traceId = generateUuid();
        static std::atomic<uint64_t> counter{0};
        if ((counter.fetch_add(1) % 100) == 0) {
            LOG_INFO("request trace_id={} method={} path={}",
                     traceId, req.method, req.path);
        }

        // 健康检查与指标接口走 Reactor 快路径，不进入线程池
        if (req.method == "GET" &&
            (req.path == "/healthz" || req.path == "/metrics")) {
            Metrics::instance().incQps();
            HttpResponse resp;
            router->route(req, &resp);
            resp.setCloseConnection(!keepAlive);
            conn->send(resp.toString());
            if (keepAlive) {
                server->onResponseSent(conn);
            } else {
                conn->shutdown();
            }
            return;
        }

        // 立即投递业务线程池，epoll 主循环马上回归继续处理其它连接
        ThreadPool* pool = (req.method == "POST" &&
                            (req.path == "/api/tasks" || req.path == "/api/tasks/direct"))
                               ? svc->uploadPool.get()
                               : svc->threadPool.get();
        bool queued = pool->tryEnqueue(
            [router, svc, server, req = std::move(req), conn = std::move(conn),
             keepAlive]() {
            Metrics::instance().incQps();
            HttpResponse resp;
            router->route(req, &resp);
            resp.setCloseConnection(!keepAlive);
            conn->send(resp.toString());
            if (keepAlive) {
                server->onResponseSent(conn);
            } else {
                conn->shutdown();
            }
        });
        if (!queued) {
            HttpResponse resp = HttpResponse::serviceUnavailable("server busy, please retry later");
            resp.setCloseConnection(true);
            conn->send(resp.toString());
            conn->shutdown();
        }
    };
}

} // namespace transcode
