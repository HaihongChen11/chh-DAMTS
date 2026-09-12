#pragma once

#include <string>

namespace transcode {

// 生成 UUID v4 字符串（36 字符，含连字符），用作 task_id
std::string generateUuid();

// 生成随机 token（用于会话鉴权）
std::string generateToken(int byte_len = 32);

// 生成随机盐（hex 字符串，16 字节 -> 32 字符）
std::string generateSalt();

} // namespace transcode
