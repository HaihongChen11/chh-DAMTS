#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "common/config.h"
#include "common/noncopyable.h"
#include "reactor/http_parser.h"
#include "service/types.h"

namespace sw { namespace redis { class Redis; } }
namespace transcode {
class MysqlPool;
class StorageService;
class MqService;
class EsService;
}

namespace transcode {

// 转码任务服务：提交（上传 + 入库 + 投递）、状态查询（Cache-Aside）、
// 预签名 URL、全文检索；同时提供 worker 侧的状态流转与元数据落库。
class TaskService : Noncopyable {
public:
    TaskService(std::shared_ptr<MysqlPool> mysql,
                std::shared_ptr<sw::redis::Redis> redis,
                std::shared_ptr<StorageService> storage,
                std::shared_ptr<MqService> mq,
                std::shared_ptr<EsService> es,
                const Config& cfg);

    // ---------------- api_server 侧 ----------------
    struct SubmitResult {
        bool ok = false;
        std::string task_id;
        std::string error;
    };
    SubmitResult submitTask(int64_t userId, const std::string& videoName, const std::string& tag,
                            const std::string& resolution, int bitrate, const FilePart& file,
                            const std::string& idempotencyKey);

    nlohmann::json getTaskStatus(int64_t userId, const std::string& taskId);
    std::string getPresignedUrl(int64_t userId, const std::string& taskId);
    std::string search(int64_t userId, const std::string& query);

    // ---------------- worker 侧 ----------------
    bool getTask(const std::string& taskId, TranscodeTask* out);
    bool markProcessing(const std::string& taskId);
    bool markSuccess(const std::string& taskId, const std::string& outputKey,
                     const std::string& thumbnailKey, const VideoMeta& meta);
    bool markFailed(const std::string& taskId, const std::string& errorMsg);
    bool incrementRetry(const std::string& taskId);
    void updateProgress(const std::string& taskId, int percent);

private:
    void invalidateCache(const std::string& taskId);

    std::shared_ptr<MysqlPool> mysql_;
    std::shared_ptr<sw::redis::Redis> redis_;
    std::shared_ptr<StorageService> storage_;
    std::shared_ptr<MqService> mq_;
    std::shared_ptr<EsService> es_;
    Config cfg_;
};

} // namespace transcode
