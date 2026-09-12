#include "reactor/http_response.h"

#include <sstream>

#include <nlohmann/json.hpp>

namespace transcode {

HttpResponse::HttpResponse() {
    headers_["Server"] = "transcode/1.0";
    headers_["Content-Type"] = "application/json; charset=utf-8";
    headers_["Connection"] = "close";
}

void HttpResponse::setStatus(int code, const std::string& msg) {
    statusCode_ = code;
    statusMessage_ = msg;
}

void HttpResponse::setHeader(const std::string& key, const std::string& value) {
    headers_[key] = value;
}

void HttpResponse::setContentType(const std::string& ct) {
    headers_["Content-Type"] = ct;
}

void HttpResponse::setBody(const std::string& body) {
    body_ = body;
    headers_["Content-Length"] = std::to_string(body_.size());
}

void HttpResponse::setCloseConnection(bool close) {
    closeConnection_ = close;
    headers_["Connection"] = close ? "close" : "keep-alive";
}

std::string HttpResponse::toString() const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode_ << " " << statusMessage_ << "\r\n";
    for (const auto& kv : headers_) {
        oss << kv.first << ": " << kv.second << "\r\n";
    }
    oss << "\r\n";
    oss << body_;
    return oss.str();
}

namespace {
std::string jsonError(int code, const std::string& msg) {
    nlohmann::json j;
    j["code"] = code;
    j["message"] = msg;
    return j.dump();
}
} // namespace

HttpResponse HttpResponse::ok(const std::string& json_body) {
    HttpResponse resp;
    resp.setBody(json_body);
    return resp;
}

HttpResponse HttpResponse::created(const std::string& json_body) {
    HttpResponse resp;
    resp.setStatus(201, "Created");
    resp.setBody(json_body);
    return resp;
}

HttpResponse HttpResponse::noContent() {
    HttpResponse resp;
    resp.setStatus(204, "No Content");
    resp.setBody("");
    return resp;
}

HttpResponse HttpResponse::badRequest(const std::string& msg) {
    HttpResponse resp;
    resp.setStatus(400, "Bad Request");
    resp.setBody(jsonError(400, msg));
    return resp;
}

HttpResponse HttpResponse::unauthorized(const std::string& msg) {
    HttpResponse resp;
    resp.setStatus(401, "Unauthorized");
    resp.setBody(jsonError(401, msg));
    return resp;
}

HttpResponse HttpResponse::forbidden(const std::string& msg) {
    HttpResponse resp;
    resp.setStatus(403, "Forbidden");
    resp.setBody(jsonError(403, msg));
    return resp;
}

HttpResponse HttpResponse::notFound(const std::string& msg) {
    HttpResponse resp;
    resp.setStatus(404, "Not Found");
    resp.setBody(jsonError(404, msg));
    return resp;
}

HttpResponse HttpResponse::tooManyRequests(const std::string& msg) {
    HttpResponse resp;
    resp.setStatus(429, "Too Many Requests");
    resp.setBody(jsonError(429, msg));
    return resp;
}

HttpResponse HttpResponse::internalError(const std::string& msg) {
    HttpResponse resp;
    resp.setStatus(500, "Internal Server Error");
    resp.setBody(jsonError(500, msg));
    return resp;
}

} // namespace transcode
