#pragma once

#include <map>
#include <string>

namespace transcode {

// HTTP 响应：组装响应报文（状态行 + 头部 + 空行 + body）。
// 手写报文构造，不依赖任何 HTTP 框架。
class HttpResponse {
public:
    HttpResponse();

    void setStatus(int code, const std::string& msg);
    void setHeader(const std::string& key, const std::string& value);
    void setContentType(const std::string& ct);
    void setBody(const std::string& body);
    void setCloseConnection(bool close);

    int statusCode() const { return statusCode_; }

    // 组装完整响应报文
    std::string toString() const;

    // 便捷构造：body 已经是 JSON 字符串
    static HttpResponse ok(const std::string& json_body);
    static HttpResponse created(const std::string& json_body);
    static HttpResponse noContent();

    // 便捷构造：将 msg 包装成 {"code":xxx,"message":"..."}
    static HttpResponse badRequest(const std::string& msg);
    static HttpResponse unauthorized(const std::string& msg);
    static HttpResponse forbidden(const std::string& msg);
    static HttpResponse notFound(const std::string& msg);
    static HttpResponse tooManyRequests(const std::string& msg);
    static HttpResponse serviceUnavailable(const std::string& msg);
    static HttpResponse internalError(const std::string& msg);

private:
    int statusCode_ = 200;
    std::string statusMessage_ = "OK";
    std::map<std::string, std::string> headers_;
    std::string body_;
    bool closeConnection_ = false;
};

} // namespace transcode
