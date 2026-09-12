#include "service/task_service.h"

#include <algorithm>
#include <chrono>

#include <unistd.h>

#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <sw/redis++/redis.h>

#include "common/logger.h"
#include "common/uuid.h"
#include "service/db.h"
#include "service/es_service.h"
#include "service/mq_service.h"
#include "service/storage_service.h"

namespace transcode {

namespace {
nlohmann::json taskToJson(const TranscodeTask& t) {
    nlohmann::json j;
    j["task_id"] = t.task_id;
    j["user_id"] = t.user_id;
    j["status"] = t.status;
    j["resolution"] = t.resolution;
    j["bitrate"] = t.bitrate;
    j["output_key"] = t.output_key;
    j["thumbnail_key"] = t.thumbnail_key;
    j["retry_count"] = t.retry_count;
    j["error_msg"] = t.error_msg;
    return j;
}

TranscodeTask rowToTask(sql::ResultSet* rs) {
    TranscodeTask t;
    t.task_id = rs->getString("task_id");
    t.user_id = rs->getInt64("user_id");
    t.source_bucket = rs->getString("source_bucket");
    t.source_key = rs->getString("source_key");
    t.output_bucket = rs->getString("output_bucket");
    t.output_key = rs->isNull("output_key") ? "" : rs->getString("output_key");
    t.thumbnail_key = rs->isNull("thumbnail_key") ? "" : rs->getString("thumbnail_key");
    t.resolution = rs->getString("resolution");
    t.bitrate = rs->getInt("bitrate");
    t.status = rs->getString("status");
    t.retry_count = rs->getInt("retry_count");
    t.error_msg = rs->isNull("error_msg") ? "" : rs->getString("error_msg");
    return t;
}
} // namespace

TaskService::TaskService(std::shared_ptr<MysqlPool> mysql,
                         std::shared_ptr<sw::redis::Redis> redis,
                         std::shared_ptr<StorageService> storage,
                         std::shared_ptr<MqService> mq,
                         std::shared_ptr<EsService> es,
                         const Config& cfg)
    : mysql_(std::move(mysql)),
      redis_(std::move(redis)),
      storage_(std::move(storage)),
      mq_(std::move(mq)),
      es_(std::move(es)),
      cfg_(cfg) {}

// ---------------- api_server 侧 ----------------

TaskService::SubmitResult TaskService::submitTask(int64_t userId, const std::string& videoName,
                                                 const std::string& tag,
                                                 const std::string& resolution, int bitrate,
                                                 const FilePart& file,
                                                 const std::string& idempotencyKey) {
    SubmitResult result;

    // 幂等校验：同一幂等 key 重复提交返回已有 task_id
    if (!idempotencyKey.empty()) {
        try {
            auto existing = redis_->get("idem:" + idempotencyKey);
            if (existing) {
                result.ok = true;
                result.task_id = *existing;
                return result;
            }
        } catch (const sw::redis::Error& e) {
            LOG_WARN("idempotency check redis error: {}", e.what());
        }
    }

    std::string taskId = generateUuid();
    std::string sourceKey = "users/" + std::to_string(userId) + "/" + taskId + "/" +
                            (file.filename.empty() ? "video" : file.filename);

    // 1. 流式上传源视频到 MinIO
    if (!storage_->uploadFile(cfg_.minio().source_bucket, sourceKey, file.temp_path)) {
        result.error = "upload source video failed";
        return result;
    }

    // 2. 事务：插入 pending 任务
    try {
        Transaction tx(mysql_.get());
        std::unique_ptr<sql::PreparedStatement> stmt(tx.conn()->prepareStatement(
            "INSERT INTO transcode_task (task_id, user_id, source_bucket, source_key, "
            "output_bucket, resolution, bitrate, status, retry_count) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, 'pending', 0)"));
        stmt->setString(1, taskId);
        stmt->setInt64(2, userId);
        stmt->setString(3, cfg_.minio().source_bucket);
        stmt->setString(4, sourceKey);
        stmt->setString(5, cfg_.minio().output_bucket);
        stmt->setString(6, resolution);
        stmt->setInt(7, bitrate);
        stmt->executeUpdate();
        tx.commit();
    } catch (const sql::SQLException& e) {
        LOG_ERROR("insert task failed: {}", e.what());
        result.error = "insert task failed";
        return result;
    }

    // 3. 投递 RabbitMQ（confirm）
    TaskMessage msg;
    msg.task_id = taskId;
    msg.retry_count = 0;
    msg.video_name = videoName;
    msg.tag = tag;
    if (!mq_->publish(msg)) {
        // 消息投递失败：任务停留在 pending，由后续对账机制兜底重投
        LOG_ERROR("publish task to mq failed: {}", taskId);
        result.error = "message queue publish failed";
        return result;
    }

    // 4. 记录幂等映射（TTL）
    if (!idempotencyKey.empty()) {
        try {
            redis_->set("idem:" + idempotencyKey, taskId, std::chrono::seconds(3600));
        } catch (const sw::redis::Error& e) {
            LOG_WARN("store idempotency key failed: {}", e.what());
        }
    }

    // 5. 清理临时文件
    if (!file.temp_path.empty()) ::unlink(file.temp_path.c_str());

    result.ok = true;
    result.task_id = taskId;
    return result;
}

nlohmann::json TaskService::getTaskStatus(int64_t userId, const std::string& taskId) {
    // Cache-Aside：先查 Redis 缓存
    try {
        auto cached = redis_->get("task:" + taskId);
        if (cached) {
            auto j = nlohmann::json::parse(*cached);
            if (j.value("user_id", 0) == userId) return j;
            return nlohmann::json{{"error", "forbidden"}};
        }
    } catch (const sw::redis::Error& e) {
        LOG_WARN("get task cache redis error: {}", e.what());
    }

    TranscodeTask t;
    try {
        auto conn = mysql_->acquire();
        std::unique_ptr<sql::PreparedStatement> stmt(
            conn->prepareStatement("SELECT * FROM transcode_task WHERE task_id = ?"));
        stmt->setString(1, taskId);
        std::unique_ptr<sql::ResultSet> rs(stmt->executeQuery());
        if (!rs->next()) return nlohmann::json{{"error", "not found"}};
        t = rowToTask(rs.get());
    } catch (const sql::SQLException& e) {
        LOG_ERROR("query task failed: {}", e.what());
        return nlohmann::json{{"error", "internal error"}};
    }

    if (t.user_id != userId) return nlohmann::json{{"error", "forbidden"}};

    nlohmann::json j = taskToJson(t);
    try {
        auto progress = redis_->get("task_progress:" + taskId);
        int pct = 0;
        if (progress) pct = std::max(0, std::min(100, std::stoi(*progress)));
        j["progress"] = (t.status == TaskStatus::kSuccess) ? 100 : pct;
    } catch (const std::exception&) {
        j["progress"] = (t.status == TaskStatus::kSuccess) ? 100 : 0;
    }
    try {
        redis_->set("task:" + taskId, j.dump(), std::chrono::seconds(60));
    } catch (const sw::redis::Error& e) {
        LOG_WARN("cache task failed: {}", e.what());
    }
    return j;
}

std::string TaskService::getPresignedUrl(int64_t userId, const std::string& taskId) {
    TranscodeTask t;
    if (!getTask(taskId, &t)) return std::string();
    if (t.user_id != userId) return std::string();
    if (t.status != TaskStatus::kSuccess || t.output_key.empty()) return std::string();
    return storage_->presignUrl(t.output_bucket, t.output_key, 3600);
}

std::string TaskService::search(int64_t /*userId*/, const std::string& query) {
    return es_->search(query);
}

// ---------------- worker 侧 ----------------

bool TaskService::getTask(const std::string& taskId, TranscodeTask* out) {
    try {
        auto conn = mysql_->acquire();
        std::unique_ptr<sql::PreparedStatement> stmt(
            conn->prepareStatement("SELECT * FROM transcode_task WHERE task_id = ?"));
        stmt->setString(1, taskId);
        std::unique_ptr<sql::ResultSet> rs(stmt->executeQuery());
        if (!rs->next()) return false;
        *out = rowToTask(rs.get());
        return true;
    } catch (const sql::SQLException& e) {
        LOG_ERROR("get task failed: {}", e.what());
        return false;
    }
}

bool TaskService::markProcessing(const std::string& taskId) {
    try {
        auto conn = mysql_->acquire();
        std::unique_ptr<sql::PreparedStatement> stmt(conn->prepareStatement(
            "UPDATE transcode_task SET status='processing' WHERE task_id = ? AND status = 'pending'"));
        stmt->setString(1, taskId);
        int affected = stmt->executeUpdate();
        if (affected > 0) invalidateCache(taskId);
        return affected > 0;
    } catch (const sql::SQLException& e) {
        LOG_ERROR("mark processing failed: {}", e.what());
        return false;
    }
}

bool TaskService::markSuccess(const std::string& taskId, const std::string& outputKey,
                              const std::string& thumbnailKey, const VideoMeta& meta) {
    try {
        Transaction tx(mysql_.get());
        // 更新任务状态
        {
            std::unique_ptr<sql::PreparedStatement> stmt(tx.conn()->prepareStatement(
                "UPDATE transcode_task SET status='success', output_key = ?, thumbnail_key = ?, "
                "finish_time = NOW() WHERE task_id = ?"));
            stmt->setString(1, outputKey);
            stmt->setString(2, thumbnailKey);
            stmt->setString(3, taskId);
            stmt->executeUpdate();
        }
        // 写入视频元数据
        {
            std::unique_ptr<sql::PreparedStatement> stmt(tx.conn()->prepareStatement(
                "INSERT INTO video_meta (task_id, video_name, tag, duration, width, height, size, thumbnail_key) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
            stmt->setString(1, taskId);
            stmt->setString(2, meta.video_name);
            stmt->setString(3, meta.tag);
            stmt->setDouble(4, meta.duration);
            stmt->setInt(5, meta.width);
            stmt->setInt(6, meta.height);
            stmt->setInt64(7, meta.size);
            stmt->setString(8, meta.thumbnail_key);
            stmt->executeUpdate();
        }
        tx.commit();
        invalidateCache(taskId);

        // 同步元数据到 ES（best-effort，失败不影响主流程）
        es_->indexMeta(meta);
        return true;
    } catch (const sql::SQLException& e) {
        LOG_ERROR("mark success failed: {}", e.what());
        return false;
    }
}

bool TaskService::markFailed(const std::string& taskId, const std::string& errorMsg) {
    try {
        auto conn = mysql_->acquire();
        std::unique_ptr<sql::PreparedStatement> stmt(conn->prepareStatement(
            "UPDATE transcode_task SET status='failed', error_msg = ?, finish_time = NOW() "
            "WHERE task_id = ?"));
        stmt->setString(1, errorMsg);
        stmt->setString(2, taskId);
        int affected = stmt->executeUpdate();
        if (affected > 0) invalidateCache(taskId);
        return affected > 0;
    } catch (const sql::SQLException& e) {
        LOG_ERROR("mark failed error: {}", e.what());
        return false;
    }
}

bool TaskService::incrementRetry(const std::string& taskId) {
    try {
        auto conn = mysql_->acquire();
        std::unique_ptr<sql::PreparedStatement> stmt(conn->prepareStatement(
            "UPDATE transcode_task SET retry_count = retry_count + 1 WHERE task_id = ?"));
        stmt->setString(1, taskId);
        return stmt->executeUpdate() > 0;
    } catch (const sql::SQLException& e) {
        LOG_ERROR("increment retry failed: {}", e.what());
        return false;
    }
}

void TaskService::updateProgress(const std::string& taskId, int percent) {
    try {
        percent = std::max(0, std::min(100, percent));
        redis_->set("task_progress:" + taskId, std::to_string(percent),
                    std::chrono::seconds(120));
    } catch (const sw::redis::Error& e) {
        LOG_WARN("update progress failed: {}", e.what());
    }
}

void TaskService::invalidateCache(const std::string& taskId) {
    try {
        redis_->del("task:" + taskId);
    } catch (const sw::redis::Error& e) {
        LOG_WARN("invalidate cache failed: {}", e.what());
    }
}

} // namespace transcode
