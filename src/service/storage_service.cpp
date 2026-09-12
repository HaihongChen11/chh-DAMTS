#include "service/storage_service.h"

#include <fstream>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadBucketRequest.h>
#include <aws/s3/model/PutObjectRequest.h>

#include "common/logger.h"

namespace transcode {

namespace {
std::string stripScheme(const std::string& url) {
    if (url.rfind("http://", 0) == 0) return url.substr(7);
    if (url.rfind("https://", 0) == 0) return url.substr(8);
    return url;
}
} // namespace

StorageService::StorageService(const Config::Minio& cfg) : cfg_(cfg) {
    Aws::S3::S3ClientConfiguration config;
    config.endpointOverride = stripScheme(cfg.endpoint);
    config.scheme = cfg.use_ssl ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
    config.region = cfg.region;
    config.verifySSL = false;
    // MinIO 使用 path-style 寻址，必须关闭 virtual hosting
    config.useVirtualAddressing = false;

    client_ = std::make_shared<Aws::S3::S3Client>(
        Aws::Auth::AWSCredentials(cfg.access_key, cfg.secret_key), nullptr, config);
}

bool StorageService::ensureBucket(const std::string& bucket) {
    Aws::S3::Model::HeadBucketRequest head;
    head.SetBucket(bucket);
    auto outcome = client_->HeadBucket(head);
    if (outcome.IsSuccess()) return true;

    Aws::S3::Model::CreateBucketRequest create;
    create.SetBucket(bucket);
    auto created = client_->CreateBucket(create);
    if (!created.IsSuccess()) {
        LOG_ERROR("create bucket {} failed: {}", bucket, created.GetError().GetMessage());
        return false;
    }
    return true;
}

bool StorageService::uploadFile(const std::string& bucket, const std::string& key,
                                const std::string& local_path) {
    Aws::S3::Model::PutObjectRequest request;
    request.SetBucket(bucket);
    request.SetKey(key);

    // 流式上传：用文件流作为 body，不把整个文件读入内存
    auto body = Aws::MakeShared<Aws::FStream>("PutObject", local_path.c_str(),
                                              std::ios_base::in | std::ios_base::binary);
    if (!body->is_open()) {
        LOG_ERROR("open upload file failed: {}", local_path);
        return false;
    }
    request.SetBody(body);

    auto outcome = client_->PutObject(request);
    if (!outcome.IsSuccess()) {
        LOG_ERROR("upload {} to {}:{} failed: {}", local_path, bucket, key,
                  outcome.GetError().GetMessage());
        return false;
    }
    return true;
}

bool StorageService::downloadFile(const std::string& bucket, const std::string& key,
                                  const std::string& local_path) {
    Aws::S3::Model::GetObjectRequest request;
    request.SetBucket(bucket);
    request.SetKey(key);

    auto outcome = client_->GetObject(request);
    if (!outcome.IsSuccess()) {
        LOG_ERROR("download {}:{} failed: {}", bucket, key, outcome.GetError().GetMessage());
        return false;
    }

    std::ofstream ofs(local_path, std::ios::binary);
    if (!ofs.is_open()) return false;
    ofs << outcome.GetResult().GetBody().rdbuf();
    ofs.close();
    return true;
}

std::string StorageService::presignUrl(const std::string& bucket, const std::string& key,
                                       int64_t expire_seconds) {
    return client_->GeneratePresignedUrl(bucket, key, Aws::Http::HttpMethod::HTTP_GET,
                                         expire_seconds);
}

} // namespace transcode
