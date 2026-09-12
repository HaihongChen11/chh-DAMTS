#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/noncopyable.h"

namespace sw { namespace redis { class Redis; } }

namespace transcode {

// 带超时防死锁的 Redis 分布式锁：
//   tryLock 使用 SET key owner NX PX ttl，原子加锁并设置过期时间；
//   unlock  使用 Lua 脚本「校验 owner 再删除」，防止误删其它持有者的锁。
// 过期时间保证持有者崩溃后锁能自动释放，避免死锁。
class DistributedLock : Noncopyable {
public:
    explicit DistributedLock(std::shared_ptr<sw::redis::Redis> redis);

    // 尝试加锁，owner_id 为唯一持有者标识（如 worker 实例 id）
    bool tryLock(const std::string& key, const std::string& owner_id, int64_t ttl_ms);
    // 释放锁，返回是否释放成功
    bool unlock(const std::string& key, const std::string& owner_id);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
};

} // namespace transcode
