#include "service/auth_service.h"

#include <cstring>

#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <openssl/evp.h>
#include <sw/redis++/redis.h>

#include "common/logger.h"
#include "common/uuid.h"
#include "service/db.h"

namespace transcode {

namespace {
std::string toHex(const unsigned char* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0x0f]);
    }
    return out;
}
} // namespace

AuthService::AuthService(std::shared_ptr<MysqlPool> mysql,
                         std::shared_ptr<sw::redis::Redis> redis,
                         int64_t token_ttl_seconds)
    : mysql_(std::move(mysql)), redis_(std::move(redis)), tokenTtl_(token_ttl_seconds) {}

std::string AuthService::hashPassword(const std::string& salt, const std::string& password) const {
    constexpr int kIterations = 100000;
    constexpr int kKeyLen = 32;
    unsigned char digest[kKeyLen];
    PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()),
        reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
        kIterations, EVP_sha256(), kKeyLen, digest);
    return toHex(digest, kKeyLen);
}

bool AuthService::registerUser(const std::string& username, const std::string& password,
                               std::string* err) {
    if (username.empty() || password.empty()) {
        if (err) *err = "username or password empty";
        return false;
    }
    std::string salt = generateSalt();
    std::string hash = hashPassword(salt, password);

    try {
        auto conn = mysql_->acquire();
        std::unique_ptr<sql::PreparedStatement> stmt(conn->prepareStatement(
            "INSERT INTO user (username, password_salt, password_hash) VALUES (?, ?, ?)"));
        stmt->setString(1, username);
        stmt->setString(2, salt);
        stmt->setString(3, hash);
        stmt->executeUpdate();
        return true;
    } catch (const sql::SQLException& e) {
        // 1062 = duplicate entry（用户名重复）
        if (e.getErrorCode() == 1062) {
            if (err) *err = "username already exists";
        } else {
            LOG_ERROR("register user failed: {}", e.what());
            if (err) *err = "internal error";
        }
        return false;
    }
}

std::string AuthService::login(const std::string& username, const std::string& password) {
    std::string salt, hash;
    int64_t userId = 0;
    try {
        auto conn = mysql_->acquire();
        std::unique_ptr<sql::PreparedStatement> stmt(conn->prepareStatement(
            "SELECT id, password_salt, password_hash FROM user WHERE username = ?"));
        stmt->setString(1, username);
        std::unique_ptr<sql::ResultSet> rs(stmt->executeQuery());
        if (!rs->next()) return std::string();
        userId = rs->getInt64("id");
        salt = rs->getString("password_salt");
        hash = rs->getString("password_hash");
    } catch (const sql::SQLException& e) {
        LOG_ERROR("login query failed: {}", e.what());
        return std::string();
    }

    if (hashPassword(salt, password) != hash) return std::string();

    // 生成 token 并写入 Redis（带 TTL）
    std::string token = generateToken();
    try {
        redis_->set("token:" + token, std::to_string(userId),
                    std::chrono::seconds(tokenTtl_));
    } catch (const sw::redis::Error& e) {
        LOG_ERROR("store token failed: {}", e.what());
        return std::string();
    }
    return token;
}

int64_t AuthService::authenticate(const std::string& token) {
    if (token.empty()) return 0;
    try {
        auto val = redis_->get("token:" + token);
        if (!val) return 0;
        return std::stoll(*val);
    } catch (const sw::redis::Error& e) {
        LOG_WARN("authenticate redis error: {}", e.what());
        return 0;
    }
}

void AuthService::logout(const std::string& token) {
    if (token.empty()) return;
    try {
        redis_->del("token:" + token);
    } catch (const sw::redis::Error& e) {
        LOG_WARN("logout redis error: {}", e.what());
    }
}

} // namespace transcode
