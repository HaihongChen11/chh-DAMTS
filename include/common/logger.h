#pragma once

#include <utility>
#include <string>

#include <spdlog/spdlog.h>

// 统一日志封装：对外暴露宏，内部基于 spdlog 的 fmt 格式串（{} 占位）。
// 通过变参模板直接把格式串与参数转发给 spdlog，避免 printf 风格的类型不匹配。
namespace transcode {

// 初始化日志：level 取值 trace/debug/info/warn/error
void initLogger(const std::string& level = "info", const std::string& log_file = "");

// 变参转发入口：fmt 为 spdlog/fmt 风格（{}），支持任意可格式化类型
template <typename... Args>
void logMessage(const char* level, const char* fmt, Args&&... args) {
    auto logger = spdlog::get("transcode");
    if (!logger) logger = spdlog::default_logger();
    if (!logger) return;
    logger->log(spdlog::level::from_str(level), fmt, std::forward<Args>(args)...);
}

#define LOG_TRACE(...) ::transcode::logMessage("trace", __VA_ARGS__)
#define LOG_DEBUG(...) ::transcode::logMessage("debug", __VA_ARGS__)
#define LOG_INFO(...)  ::transcode::logMessage("info", __VA_ARGS__)
#define LOG_WARN(...)  ::transcode::logMessage("warn", __VA_ARGS__)
#define LOG_ERROR(...) ::transcode::logMessage("error", __VA_ARGS__)

} // namespace transcode
