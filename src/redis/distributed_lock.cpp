#include "redis/distributed_lock.h"

#include <chrono>

#include <sw/redis++/redis.h>

#include "common/logger.h"

namespace transcode {

namespace {
// 释放锁 Lua 脚本：只有持有者本人才能删除锁
const char* kUnlockScript = R"LUA(
if redis.call('GET', KEYS[1]) == ARGV[1] then
    return redis.call('DEL', KEYS[1])
else
    return 0
end
)LUA";
}

DistributedLock::DistributedLock(std::shared_ptr<sw::redis::Redis> redis)
    : redis_(std::move(redis)) {}

bool DistributedLock::tryLock(const std::string& key, const std::string& owner_id,
                              int64_t ttl_ms) {
    try {
        // SET key owner NX PX ttl —— 原子地「仅当不存在时设置」并带过期时间
        return redis_->set(key, owner_id, std::chrono::milliseconds(ttl_ms),
                           sw::redis::UpdateType::NOT_EXIST);
    } catch (const sw::redis::Error& e) {
        LOG_ERROR("distributed lock tryLock error: {}", e.what());
        return false;
    }
}

bool DistributedLock::unlock(const std::string& key, const std::string& owner_id) {
    try {
        auto ret = redis_->eval<long long>(kUnlockScript, {key}, {owner_id});
        return ret == 1;
    } catch (const sw::redis::Error& e) {
        LOG_ERROR("distributed lock unlock error: {}", e.what());
        return false;
    }
}

} // namespace transcode
