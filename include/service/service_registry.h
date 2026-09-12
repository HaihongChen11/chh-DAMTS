#pragma once

#include <memory>

#include "common/config.h"
#include "common/noncopyable.h"
#include "pool/thread_pool.h"
#include "redis/distributed_lock.h"
#include "redis/rate_limiter.h"
#include "service/auth_service.h"
#include "service/db.h"
#include "service/es_service.h"
#include "service/mq_service.h"
#include "service/storage_service.h"
#include "service/task_service.h"

namespace sw { namespace redis { class Redis; } }

namespace transcode {

// 服务组合根：根据配置创建并持有所有依赖（中间件客户端、组件、业务服务），
// api_server 与 transcode_worker 共用，按需使用其中的子集。
class ServiceRegistry : Noncopyable {
public:
    bool init(const Config& cfg);
    void shutdown();

    Config cfg;
    std::shared_ptr<ThreadPool> threadPool;
    std::shared_ptr<sw::redis::Redis> redis;
    std::shared_ptr<MysqlPool> mysql;
    std::shared_ptr<StorageService> storage;
    std::shared_ptr<MqService> mq;
    std::shared_ptr<EsService> es;
    std::shared_ptr<RateLimiter> rateLimiter;
    std::shared_ptr<DistributedLock> distributedLock;
    std::shared_ptr<AuthService> auth;
    std::shared_ptr<TaskService> task;
};

} // namespace transcode
