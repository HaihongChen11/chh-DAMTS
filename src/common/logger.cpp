#include "common/logger.h"

#include <memory>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace transcode {

void initLogger(const std::string& level, const std::string& log_file) {
    try {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        if (!log_file.empty()) {
            sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, true));
        }
        auto logger = std::make_shared<spdlog::logger>("transcode", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::from_str(level));
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::warn);
    } catch (const spdlog::spdlog_ex&) {
        // 日志初始化失败不影响主流程
    }
}

} // namespace transcode
