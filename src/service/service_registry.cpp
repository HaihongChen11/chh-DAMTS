#include "service/service_registry.h"

#include <sw/redis++/redis.h>

#include "common/logger.h"

namespace transcode {

bool ServiceRegistry::init(const Config& cfg) {
    this->cfg = cfg;

    // Redis 客户端（内部自带连接池）
    try {
        sw::redis::ConnectionOptions opts;
        opts.host = cfg.redis().host;
        opts.port = cfg.redis().port;
        opts.password = cfg.redis().password;
        opts.db = cfg.redis().db;
        sw::redis::ConnectionPoolOptions poolOpts;
        poolOpts.size = cfg.redis().pool_size;
        redis = std::make_shared<sw::redis::Redis>(opts, poolOpts);
        redis->ping();
    } catch (const std::exception& e) {
        LOG_ERROR("redis connect failed: {}", e.what());
        return false;
    }

    // MySQL 连接池
    mysql = std::make_shared<MysqlPool>(cfg.mysql());

    // 业务线程池
    threadPool = std::make_shared<ThreadPool>(cfg.server().thread_pool_size);
    threadPool->start();

    // MinIO 对象存储
    storage = std::make_shared<StorageService>(cfg.minio());
    storage->ensureBucket(cfg.minio().source_bucket);
    storage->ensureBucket(cfg.minio().output_bucket);

    // RabbitMQ
    mq = std::make_shared<MqService>(cfg.rabbitmq());
    if (!mq->connect()) {
        LOG_ERROR("rabbitmq init failed");
        return false;
    }

    // Elasticsearch
    es = std::make_shared<EsService>(cfg.es());

    // Redis 组件
    rateLimiter = std::make_shared<RateLimiter>(
        redis, cfg.redis().rate_limit_window_seconds, cfg.redis().rate_limit_max_requests);
    distributedLock = std::make_shared<DistributedLock>(redis);

    // 业务服务
    auth = std::make_shared<AuthService>(mysql, redis, cfg.redis().token_ttl_seconds);
    task = std::make_shared<TaskService>(mysql, redis, storage, mq, es, cfg);

    LOG_INFO("service registry initialized");
    return true;
}

void ServiceRegistry::shutdown() {
    if (mq) mq->stop();
    if (threadPool) threadPool->stop();
}

} // namespace transcode
