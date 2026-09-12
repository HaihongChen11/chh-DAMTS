#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/noncopyable.h"

namespace sw { namespace redis { class Redis; } }
namespace transcode { class MysqlPool; }

namespace transcode {

// 用户注册 / 登录 / token 鉴权。
// 密码加盐哈希存储（SHA256(salt + password)）；token 存 Redis 带 TTL。
class AuthService : Noncopyable {
public:
    AuthService(std::shared_ptr<MysqlPool> mysql,
                std::shared_ptr<sw::redis::Redis> redis,
                int64_t token_ttl_seconds);

    // 注册：成功返回 true；用户名重复返回 false 并设置 err
    bool registerUser(const std::string& username, const std::string& password, std::string* err);
    // 登录：成功返回 token；失败返回空串
    std::string login(const std::string& username, const std::string& password);
    // 鉴权：token -> user_id；无效返回 0
    int64_t authenticate(const std::string& token);
    void logout(const std::string& token);

private:
    std::string hashPassword(const std::string& salt, const std::string& password) const;

    std::shared_ptr<MysqlPool> mysql_;
    std::shared_ptr<sw::redis::Redis> redis_;
    int64_t tokenTtl_;
};

} // namespace transcode
