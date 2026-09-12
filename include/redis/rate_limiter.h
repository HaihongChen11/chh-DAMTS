#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/noncopyable.h"

namespace sw { namespace redis { class Redis; } }

namespace transcode {

// Redis 滑动窗口限流：基于 ZSET + Lua 脚本原子执行，避免竞态。
// 以「当前时间」为 score 插入请求记录，窗口滑动删除过期记录，统计窗口内数量。
class RateLimiter : Noncopyable {
public:
    RateLimiter(std::shared_ptr<sw::redis::Redis> redis,
                int64_t window_seconds, int64_t max_requests);

    // 判断 key 是否允许通过；true=放行，false=超限
    bool allow(const std::string& key);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
    int64_t windowSeconds_;
    int64_t maxRequests_;
};

} // namespace transcode
