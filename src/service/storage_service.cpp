#include "service/storage_service.h"

#include <fstream>
#include <sys/stat.h>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadBucketRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/core/utils/stream/SimpleStreamBuf.h>

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
    // 大文件使用 S3 Multipart Upload，分片并行上传与断点续传由 SDK 底层支持
    const size_t kMultipartThreshold = 5 * 1024 * 1024;
    struct stat st;
    if (::stat(local_path.c_str(), &st) == 0 &&
        static_cast<size_t>(st.st_size) >= kMultipartThreshold) {
        return uploadFileMultipart(bucket, key, local_path);
    }

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

bool StorageService::uploadFileMultipart(const std::string& bucket,
                                         const std::string& key,
                                         const std::string& local_path) {
    std::ifstream ifs(local_path, std::ios::binary);
    if (!ifs.is_open()) {
        LOG_ERROR("open upload file failed: {}", local_path);
        return false;
    }

    Aws::S3::Model::CreateMultipartUploadRequest create;
    create.SetBucket(bucket);
    create.SetKey(key);
    auto createOutcome = client_->CreateMultipartUpload(create);
    if (!createOutcome.IsSuccess()) {
        LOG_ERROR("create multipart upload failed: {}",
                  createOutcome.GetError().GetMessage());
        return false;
    }
    const std::string uploadId = createOutcome.GetResult().GetUploadId();

    constexpr size_t kPartSize = 8 * 1024 * 1024;
    std::vector<char> buffer(kPartSize);
    Aws::Vector<Aws::S3::Model::CompletedPart> completedParts;
    int partNumber = 1;

    while (ifs) {
        ifs.read(buffer.data(), buffer.size());
        std::streamsize got = ifs.gcount();
        if (got <= 0) break;

        auto body = Aws::MakeShared<Aws::StringStream>("UploadPart");
        body->write(buffer.data(), got);

        Aws::S3::Model::UploadPartRequest part;
        part.SetBucket(bucket);
        part.SetKey(key);
        part.SetPartNumber(partNumber);
        part.SetUploadId(uploadId);
        part.SetContentLength(static_cast<long long>(got));
        part.SetBody(body);

        auto partOutcome = client_->UploadPart(part);
        if (!partOutcome.IsSuccess()) {
            LOG_ERROR("upload part {} failed: {}", partNumber,
                      partOutcome.GetError().GetMessage());
            Aws::S3::Model::AbortMultipartUploadRequest abort;
            abort.SetBucket(bucket);
            abort.SetKey(key);
            abort.SetUploadId(uploadId);
            client_->AbortMultipartUpload(abort);
            return false;
        }

        Aws::S3::Model::CompletedPart completed;
        completed.SetPartNumber(partNumber);
        completed.SetETag(partOutcome.GetResult().GetETag());
        completedParts.push_back(completed);
        ++partNumber;
    }

    Aws::S3::Model::CompletedMultipartUpload completedUpload;
    completedUpload.SetParts(completedParts);
    Aws::S3::Model::CompleteMultipartUploadRequest complete;
    complete.SetBucket(bucket);
    complete.SetKey(key);
    complete.SetUploadId(uploadId);
    complete.SetMultipartUpload(completedUpload);
    auto completeOutcome = client_->CompleteMultipartUpload(complete);
    if (!completeOutcome.IsSuccess()) {
        LOG_ERROR("complete multipart upload failed: {}",
                  completeOutcome.GetError().GetMessage());
        Aws::S3::Model::AbortMultipartUploadRequest abort;
        abort.SetBucket(bucket);
        abort.SetKey(key);
        abort.SetUploadId(uploadId);
        client_->AbortMultipartUpload(abort);
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

std::string StorageService::presignUploadUrl(const std::string& bucket, const std::string& key,
                                             int64_t expire_seconds) {
    return client_->GeneratePresignedUrl(bucket, key, Aws::Http::HttpMethod::HTTP_PUT,
                                         expire_seconds);
}

} // namespace transcode
