#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "nlohmann/json.hpp"

namespace transcode {

// 基于 nlohmann::json 的配置加载器，提供类型安全的 getter。
// 全部配置集中在一个 config.json 中，进程启动时一次性加载。
class Config {
public:
    // 从 json 文件加载，失败返回 false
    bool load(const std::string& path);

    // 通过点分路径读取，例如 "server.port"，缺失时返回默认值
    template <typename T>
    T get(const std::string& dot_path, T default_value) const {
        try {
            const nlohmann::json* node = resolve(dot_path);
            if (node == nullptr) return default_value;
            return node->get<T>();
        } catch (...) {
            return default_value;
        }
    }

    // 常用项便捷访问
    struct Server {
        std::string host;
        uint16_t port = 8080;
        int thread_pool_size = 8;
        int max_connections = 4096;
        int64_t connection_timeout_ms = 60000;
        int64_t max_upload_bytes = 2147483648LL;
    };
    struct Mysql {
        std::string host, user, password, database;
        int port = 3306;
        int pool_size = 8;
    };
    struct Redis {
        std::string host, password;
        int port = 6379;
        int db = 0;
        int pool_size = 8;
        int64_t token_ttl_seconds = 7200;
        int64_t rate_limit_window_seconds = 60;
        int64_t rate_limit_max_requests = 100;
        int64_t lock_ttl_ms = 30000;
    };
    struct RabbitMq {
        std::string host, user, password, vhost;
        int port = 5672;
        std::string exchange, queue, routing_key;
        std::string dead_letter_exchange, dead_letter_queue;
        int prefetch_count = 1;
    };
    struct Minio {
        std::string endpoint, region, access_key, secret_key;
        std::string source_bucket, output_bucket, thumbnail_bucket;
        bool use_ssl = false;
    };
    struct Elasticsearch {
        std::vector<std::string> hosts;
        std::string index = "video_meta";
    };
    struct Worker {
        int max_retry = 3;
        int concurrency = 4;
        int shard_count = 0;
        std::string ffmpeg_path = "ffmpeg";
        int default_bitrate = 1500000;
    };
    struct Metrics {
        std::string bind = "0.0.0.0";
        int port = 9100;
    };

    const Server& server() const { return server_; }
    const Mysql& mysql() const { return mysql_; }
    const Redis& redis() const { return redis_; }
    const RabbitMq& rabbitmq() const { return rabbitmq_; }
    const Minio& minio() const { return minio_; }
    const Elasticsearch& es() const { return es_; }
    const Worker& worker() const { return worker_; }
    const Metrics& metrics() const { return metrics_; }

    const nlohmann::json& raw() const { return root_; }

private:
    const nlohmann::json* resolve(const std::string& dot_path) const;

    nlohmann::json root_;
    Server server_;
    Mysql mysql_;
    Redis redis_;
    RabbitMq rabbitmq_;
    Minio minio_;
    Elasticsearch es_;
    Worker worker_;
    Metrics metrics_;
};

} // namespace transcode
