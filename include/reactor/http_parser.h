#pragma once

#include <map>
#include <string>

#include "common/noncopyable.h"
#include "reactor/buffer.h"

namespace transcode {

// 上传文件（multipart 文件 part 落盘结果）
struct FilePart {
    std::string field_name;   // 表单字段名，如 "file"
    std::string filename;     // 原始文件名
    std::string content_type; // 文件 MIME 类型
    std::string temp_path;    // 流式落盘的临时文件路径
    size_t size = 0;          // 文件字节数
};

// 解析完成的 HTTP 请求
struct HttpRequest {
    std::string method;   // GET / POST / ...
    std::string path;     // 不含 query
    std::string query;    // 原始 query 串
    std::string version;  // HTTP/1.1
    std::map<std::string, std::string> headers;
    std::string body;                            // 普通请求体（JSON 等）
    std::map<std::string, std::string> form_fields; // multipart 普通字段
    FilePart file;
    bool has_file = false;

    std::string getHeader(const std::string& key) const {
        auto it = headers.find(key);
        return it == headers.end() ? std::string() : it->second;
    }

    // 解析 query 参数
    std::map<std::string, std::string> queryParams() const;
};

enum class ParseResult { kNeedMore, kComplete, kError };

// 手写 HTTP 解析器：状态机逐段解析请求行、请求头、请求体。
// 关键设计：multipart/form-data 大文件采用流式解析，文件内容直接落盘临时文件，
// 不把完整文件加载进内存。
class HttpParser : Noncopyable {
public:
    HttpParser();
    ~HttpParser();

    // 从连接输入缓冲中消费数据，返回解析状态
    ParseResult parse(Buffer* buf);

    HttpRequest& request() { return request_; }
    const HttpRequest& request() const { return request_; }

    void reset();

    // 解析出错时是否有错误信息
    bool hasError() const { return hasError_; }
    const std::string& error() const { return error_; }

private:
    // 内部阶段返回：kDone 表示本阶段完成、可进入下一阶段
    enum class Step { kNeedMore, kDone, kError };
    Step parseRequestLine(Buffer* buf);
    Step parseHeaders(Buffer* buf);
    Step parseBody(Buffer* buf);
    Step parseMultipart(Buffer* buf);

    void parseMultipartHeaders();
    void setError(const std::string& msg);
    std::string extractBoundary() const;
    bool openTempFile();
    void writeToFile(const char* data, size_t len);
    void finalizeFilePart();

    enum State { kRequestLine, kHeaders, kBody, kMultipart };
    State state_ = kRequestLine;

    HttpRequest request_;

    size_t contentLength_ = 0;
    size_t bodyRead_ = 0;

    // multipart 内部状态
    enum MpState { kMpExpectBoundary, kMpPartHeaders, kMpPartContent, kMpDone };
    MpState mpState_ = kMpExpectBoundary;
    bool mpFirstBoundary_ = true;
    std::string boundary_;
    std::string mpHeaderBlock_;
    bool isFilePart_ = false;
    std::string fieldName_;
    std::string fieldValue_;
    std::string tmpPath_;
    int tmpFd_ = -1;
    size_t fileSize_ = 0;

    bool hasError_ = false;
    std::string error_;
};

} // namespace transcode
