#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include <unistd.h>

#include <aws/core/Aws.h>

#include "common/config.h"
#include "common/logger.h"
#include "common/uuid.h"
#include "ffmpeg/ffmpeg_helper.h"
#include "metrics/metrics.h"
#include "service/service_registry.h"
#include "service/task_service.h"
#include "service/types.h"

namespace {
std::atomic<bool> g_running{true};
std::string g_workerId;

void signalHandler(int /*sig*/) {
    g_running = false;
}
} // namespace

// 处理单个转码任务（在业务线程池线程中执行，可阻塞）
void processTask(transcode::ServiceRegistry* svc, const transcode::TaskMessage& msg,
                 uint64_t deliveryTag) {
    LOG_INFO("worker trace_id={} task_id={} retry={}",
             msg.trace_id, msg.task_id, msg.retry_count);
    const std::string lockKey = "lock:task:" + msg.task_id;

    // 1. 抢占 Redis 分布式锁，防止多 Worker 重复消费同一任务
    if (!svc->distributedLock->tryLock(lockKey, g_workerId, svc->cfg.redis().lock_ttl_ms)) {
        // 其它 worker 正在处理，直接 ack 避免重复
        svc->mq->ack(deliveryTag);
        return;
    }

    // 2. 读取任务
    transcode::TranscodeTask task;
    if (!svc->task->getTask(msg.task_id, &task)) {
        svc->mq->ack(deliveryTag);
        svc->distributedLock->unlock(lockKey, g_workerId);
        return;
    }

    // 3. 状态 pending -> processing
    svc->task->markProcessing(task.task_id);
    LOG_INFO("worker processing task_id={} trace_id={}", task.task_id, msg.trace_id);
    transcode::Metrics::instance().incInFlight();

    // 4. 从 MinIO 拉取源视频
    std::string srcPath = "/tmp/transcode_src_" + task.task_id + ".mp4";
    std::string outPath = "/tmp/transcode_out_" + task.task_id + ".mp4";
    std::string thumbPath = "/tmp/transcode_thumb_" + task.task_id + ".jpg";

    bool ok = svc->storage->downloadFile(task.source_bucket, task.source_key, srcPath);
    std::string err = ok ? "" : "download source failed";

    // 5. FFmpeg 转码（调用底层 C 库，不是 system 命令行）
    transcode::MediaInfo info;
    if (ok) {
        transcode::TranscodeParams params{srcPath, outPath, task.resolution, task.bitrate};
        params.shard_count = svc->cfg.worker().shard_count;
        params.progress = [svc, &task](int percent) {
            svc->task->updateProgress(task.task_id, percent);
        };
        ok = transcode::transcodeVideoSharded(params, &info, &err);
    }

    // 6. 截取缩略图（best-effort，失败不影响主流程）
    if (ok) {
        transcode::extractThumbnail(srcPath, thumbPath, 320, 180, 1.0, &err);
    }

    // 7. 产物回写 MinIO
    std::string outputKey = "outputs/" + task.task_id + "/output.mp4";
    std::string thumbKey = "outputs/" + task.task_id + "/thumbnail.jpg";
    if (ok) ok = svc->storage->uploadFile(task.output_bucket, outputKey, outPath);
    if (ok) svc->storage->uploadFile(svc->cfg.minio().thumbnail_bucket, thumbKey, thumbPath);

    if (ok) {
        // 8. 成功：更新 MySQL（事务），同步元数据到 ES，手动 ACK
        transcode::VideoMeta meta;
        meta.task_id = task.task_id;
        meta.video_name = msg.video_name;
        meta.tag = msg.tag;
        meta.duration = info.duration;
        meta.width = info.width;
        meta.height = info.height;
        meta.size = info.size;
        meta.thumbnail_key = thumbKey;
        svc->task->markSuccess(task.task_id, outputKey, thumbKey, meta);
        svc->mq->ack(deliveryTag);
        transcode::Metrics::instance().incTaskSuccess();
        LOG_INFO("task {} transcode success", task.task_id);
    } else {
        // 9. 失败：重试；超过最大重试次数进入死信队列
        svc->task->incrementRetry(task.task_id);
        int retryCount = msg.retry_count + 1;
        if (retryCount < svc->cfg.worker().max_retry) {
            transcode::TaskMessage retry{task.task_id, retryCount, msg.video_name, msg.tag,
                                         msg.trace_id};
            svc->mq->publish(retry);   // 重新投递
            svc->mq->ack(deliveryTag); // 原始消息确认，新消息重新入队
            LOG_WARN("task {} retry {}/{}", task.task_id, retryCount, svc->cfg.worker().max_retry);
        } else {
            svc->task->markFailed(task.task_id, err);
            svc->mq->rejectToDeadLetter(deliveryTag); // 进入死信队列
            transcode::Metrics::instance().incTaskFailed();
            LOG_ERROR("task {} failed after {} retries: {}", task.task_id, retryCount, err);
        }
    }

    // 清理临时文件 + 释放锁
    ::unlink(srcPath.c_str());
    ::unlink(outPath.c_str());
    ::unlink(thumbPath.c_str());
    transcode::Metrics::instance().decInFlight();
    svc->distributedLock->unlock(lockKey, g_workerId);
}

int main(int argc, char* argv[]) {
    std::string configPath = (argc > 1) ? argv[1] : "config.json";
    transcode::initLogger("info");
    transcode::Metrics::instance().init();

    Aws::SDKOptions awsOptions;
    Aws::InitAPI(awsOptions);

    transcode::Config cfg;
    if (!cfg.load(configPath)) {
        Aws::ShutdownAPI(awsOptions);
        return 1;
    }

    transcode::ServiceRegistry svc;
    if (!svc.init(cfg)) {
        Aws::ShutdownAPI(awsOptions);
        return 1;
    }

    g_workerId = "worker-" + transcode::generateUuid();

    // 消费回调运行在 MQ 事件线程，投递到业务线程池执行（不阻塞 MQ 连接心跳）
    svc.mq->startConsume([&svc](const transcode::TaskMessage& msg, uint64_t tag) {
        svc.threadPool->enqueue([&svc, msg, tag]() { processTask(&svc, msg, tag); });
    });

    LOG_INFO("transcode_worker {} started, waiting tasks...", g_workerId);

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    LOG_INFO("transcode_worker {} shutting down", g_workerId);
    svc.shutdown();
    Aws::ShutdownAPI(awsOptions);
    return 0;
}
