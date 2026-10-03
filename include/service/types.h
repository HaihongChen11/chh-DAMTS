#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace transcode {

// 转码任务（对应 MySQL transcode_task 表）
struct TranscodeTask {
    std::string task_id;
    int64_t user_id = 0;
    std::string source_bucket;
    std::string source_key;
    std::string output_bucket;
    std::string output_key;
    std::string thumbnail_key;
    std::string resolution;
    int bitrate = 0;
    std::string status; // pending / processing / success / failed
    int retry_count = 0;
    std::string error_msg;
};

// 视频元数据（对应 MySQL video_meta 表，也用于同步 ES）
struct VideoMeta {
    std::string task_id;
    std::string video_name;
    std::string tag;
    double duration = 0.0;
    int width = 0;
    int height = 0;
    int64_t size = 0;
    std::string thumbnail_key;

    nlohmann::json toJson() const {
        nlohmann::json j;
        j["task_id"] = task_id;
        j["video_name"] = video_name;
        j["tag"] = tag;
        j["duration"] = duration;
        j["width"] = width;
        j["height"] = height;
        j["size"] = size;
        j["thumbnail_key"] = thumbnail_key;
        return j;
    }
};

// RabbitMQ 任务消息（task_id + 重试次数 + 元信息，完整任务状态以 DB 为准）
struct TaskMessage {
    std::string task_id;
    int retry_count = 0;
    std::string video_name;
    std::string tag;
    std::string trace_id;

    nlohmann::json toJson() const {
        nlohmann::json j;
        j["task_id"] = task_id;
        j["retry_count"] = retry_count;
        j["video_name"] = video_name;
        j["tag"] = tag;
        j["trace_id"] = trace_id;
        return j;
    }

    static TaskMessage fromJson(const nlohmann::json& j) {
        TaskMessage m;
        m.task_id = j.value("task_id", std::string());
        m.retry_count = j.value("retry_count", 0);
        m.video_name = j.value("video_name", std::string());
        m.tag = j.value("tag", std::string());
        m.trace_id = j.value("trace_id", std::string());
        return m;
    }
};

// 转码任务状态常量
namespace TaskStatus {
constexpr const char* kPending = "pending";
constexpr const char* kProcessing = "processing";
constexpr const char* kSuccess = "success";
constexpr const char* kFailed = "failed";
} // namespace TaskStatus

} // namespace transcode
