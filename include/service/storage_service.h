#pragma once

#include <memory>
#include <string>

#include "common/config.h"
#include "common/noncopyable.h"

namespace Aws { namespace S3 { class S3Client; } }

namespace transcode {

// MinIO 对象存储封装（基于 AWS S3 SDK，MinIO 兼容 S3 协议）。
// 上传/下载均以文件流方式进行，不把完整文件加载进内存。
class StorageService : Noncopyable {
public:
    explicit StorageService(const Config::Minio& cfg);

    bool ensureBucket(const std::string& bucket);
    bool uploadFile(const std::string& bucket, const std::string& key, const std::string& local_path);
    bool downloadFile(const std::string& bucket, const std::string& key, const std::string& local_path);
    // 生成预签名 URL，用于临时下载/预览
    std::string presignUrl(const std::string& bucket, const std::string& key, int64_t expire_seconds = 3600);

private:
    Config::Minio cfg_;
    std::shared_ptr<Aws::S3::S3Client> client_;
};

} // namespace transcode
