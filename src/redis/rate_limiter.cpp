#include "redis/rate_limiter.h"

#include <chrono>

#include <sw/redis++/redis.h>

#include "common/logger.h"

namespace transcode {

namespace {
// 滑动窗口限流 Lua 脚本（原子执行）：
//   ZREMRANGEBYSCORE 清理窗口外的旧请求
//   ZCARD 统计窗口内请求数，超过上限拒绝
//   ZADD 记录本次请求，PEXPIRE 保证 key 过期回收
const char* kSlidingWindowScript = R"LUA(
local key = KEYS[1]
local now = tonumber(ARGV[1])
local window_ms = tonumber(ARGV[2])
local limit = tonumber(ARGV[3])
redis.call('ZREMRANGEBYSCORE', key, 0, now - window_ms)
local count = redis.call('ZCARD', key)
if count >= limit then
    return 0
end
redis.call('ZADD', key, now, now .. '-' .. math.random(1000000))
redis.call('PEXPIRE', key, window_ms)
return 1
)LUA";

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

RateLimiter::RateLimiter(std::shared_ptr<sw::redis::Redis> redis,
                         int64_t window_seconds, int64_t max_requests)
    : redis_(std::move(redis)), windowSeconds_(window_seconds), maxRequests_(max_requests) {}

bool RateLimiter::allow(const std::string& key) {
    try {
        auto ret = redis_->eval<long long>(
            kSlidingWindowScript,
            {key},
            {std::to_string(nowMs()),
             std::to_string(windowSeconds_ * 1000),
             std::to_string(maxRequests_)});
        return ret == 1;
    } catch (const sw::redis::Error& e) {
        // Redis 异常时 fail-open（放行），避免限流组件故障导致服务整体不可用
        LOG_WARN("rate limiter redis error, fail-open: {}", e.what());
        return true;
    }
}

} // namespace transcode
