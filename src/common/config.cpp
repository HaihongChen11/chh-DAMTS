#include "common/config.h"

#include <fstream>
#include <sstream>

#include "common/logger.h"

namespace transcode {

namespace {
// 按 '.' 切分路径
std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '.') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}
} // namespace

const nlohmann::json* Config::resolve(const std::string& dot_path) const {
    auto parts = splitPath(dot_path);
    const nlohmann::json* node = &root_;
    for (const auto& p : parts) {
        if (!node->is_object() || !node->contains(p)) return nullptr;
        node = &(*node)[p];
    }
    return node;
}

bool Config::load(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        LOG_ERROR("failed to open config file: {}", path);
        return false;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    try {
        root_ = nlohmann::json::parse(ss.str());
    } catch (const std::exception& e) {
        LOG_ERROR("failed to parse config json: {}", e.what());
        return false;
    }

    // 解析到结构体
    server_.host = get<std::string>("server.host", "0.0.0.0");
    server_.port = get<uint16_t>("server.port", 8080);
    server_.thread_pool_size = get<int>("server.thread_pool_size", 8);
    server_.max_connections = get<int>("server.max_connections", 4096);
    server_.connection_timeout_ms = get<int64_t>("server.connection_timeout_ms", 60000);
    server_.max_upload_bytes = get<int64_t>("server.max_upload_bytes", 2147483648LL);

    mysql_.host = get<std::string>("mysql.host", "127.0.0.1");
    mysql_.port = get<int>("mysql.port", 3306);
    mysql_.user = get<std::string>("mysql.user", "root");
    mysql_.password = get<std::string>("mysql.password", "");
    mysql_.database = get<std::string>("mysql.database", "transcode");
    mysql_.pool_size = get<int>("mysql.pool_size", 8);

    redis_.host = get<std::string>("redis.host", "127.0.0.1");
    redis_.port = get<int>("redis.port", 6379);
    redis_.password = get<std::string>("redis.password", "");
    redis_.db = get<int>("redis.db", 0);
    redis_.pool_size = get<int>("redis.pool_size", 8);
    redis_.token_ttl_seconds = get<int64_t>("redis.token_ttl_seconds", 7200);
    redis_.rate_limit_window_seconds = get<int64_t>("redis.rate_limit_window_seconds", 60);
    redis_.rate_limit_max_requests = get<int64_t>("redis.rate_limit_max_requests", 100);
    redis_.lock_ttl_ms = get<int64_t>("redis.lock_ttl_ms", 30000);

    rabbitmq_.host = get<std::string>("rabbitmq.host", "127.0.0.1");
    rabbitmq_.port = get<int>("rabbitmq.port", 5672);
    rabbitmq_.user = get<std::string>("rabbitmq.user", "guest");
    rabbitmq_.password = get<std::string>("rabbitmq.password", "guest");
    rabbitmq_.vhost = get<std::string>("rabbitmq.vhost", "/");
    rabbitmq_.exchange = get<std::string>("rabbitmq.exchange", "transcode.exchange");
    rabbitmq_.queue = get<std::string>("rabbitmq.queue", "transcode_task_queue");
    rabbitmq_.routing_key = get<std::string>("rabbitmq.routing_key", "transcode.task");
    rabbitmq_.dead_letter_exchange = get<std::string>("rabbitmq.dead_letter_exchange", "transcode.dlx");
    rabbitmq_.dead_letter_queue = get<std::string>("rabbitmq.dead_letter_queue", "transcode_dead_letter_queue");
    rabbitmq_.prefetch_count = get<int>("rabbitmq.prefetch_count", 1);

    minio_.endpoint = get<std::string>("minio.endpoint", "http://127.0.0.1:9000");
    minio_.region = get<std::string>("minio.region", "us-east-1");
    minio_.access_key = get<std::string>("minio.access_key", "");
    minio_.secret_key = get<std::string>("minio.secret_key", "");
    minio_.source_bucket = get<std::string>("minio.source_bucket", "source-videos");
    minio_.output_bucket = get<std::string>("minio.output_bucket", "output-videos");
    minio_.thumbnail_bucket = get<std::string>("minio.thumbnail_bucket", "output-videos");
    minio_.use_ssl = get<bool>("minio.use_ssl", false);

    es_.hosts = get<std::vector<std::string>>("elasticsearch.hosts", {"http://127.0.0.1:9200"});
    es_.index = get<std::string>("elasticsearch.index", "video_meta");

    worker_.max_retry = get<int>("worker.max_retry", 3);
    worker_.concurrency = get<int>("worker.concurrency", 4);
    worker_.ffmpeg_path = get<std::string>("worker.ffmpeg_path", "ffmpeg");
    worker_.default_bitrate = get<int>("worker.default_bitrate", 1500000);

    metrics_.bind = get<std::string>("metrics.bind", "0.0.0.0");
    metrics_.port = get<int>("metrics.port", 9100);

    LOG_INFO("config loaded: server={}:{} mysql={}:{} redis={}:{} mq={}:{}",
             server_.host, server_.port, mysql_.host, mysql_.port,
             redis_.host, redis_.port, rabbitmq_.host, rabbitmq_.port);
    return true;
}

} // namespace transcode
