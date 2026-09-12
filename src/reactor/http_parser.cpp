#include "reactor/http_parser.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

#include "common/logger.h"
#include "common/uuid.h"

namespace transcode {

namespace {

constexpr size_t kMaxRequestLine = 8192;
constexpr size_t kMaxHeaderBlock = 64 * 1024;
constexpr size_t kMaxFieldValue = 64 * 1024;

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// 去掉首尾引号
std::string unquote(std::string_view s) {
    std::string t = trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        t = t.substr(1, t.size() - 2);
    }
    return t;
}

int urlDecode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (c == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    return 0;
}

} // namespace

std::map<std::string, std::string> HttpRequest::queryParams() const {
    std::map<std::string, std::string> params;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        std::string kv = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        size_t eq = kv.find('=');
        if (eq == std::string::npos) {
            std::string v;
            urlDecode(kv, v);
            if (!kv.empty()) params[toLower(kv)] = v;
        } else {
            std::string k, v;
            urlDecode(kv.substr(0, eq), k);
            urlDecode(kv.substr(eq + 1), v);
            params[toLower(k)] = v;
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return params;
}

HttpParser::HttpParser() = default;

HttpParser::~HttpParser() {
    if (tmpFd_ >= 0) ::close(tmpFd_);
}

void HttpParser::setError(const std::string& msg) {
    hasError_ = true;
    error_ = msg;
}

void HttpParser::reset() {
    if (tmpFd_ >= 0) {
        ::close(tmpFd_);
        tmpFd_ = -1;
        if (!tmpPath_.empty()) ::unlink(tmpPath_.c_str());
    }
    state_ = kRequestLine;
    request_ = HttpRequest();
    contentLength_ = 0;
    bodyRead_ = 0;
    mpState_ = kMpExpectBoundary;
    mpFirstBoundary_ = true;
    boundary_.clear();
    mpHeaderBlock_.clear();
    isFilePart_ = false;
    fieldName_.clear();
    fieldValue_.clear();
    tmpPath_.clear();
    fileSize_ = 0;
    hasError_ = false;
    error_.clear();
}

ParseResult HttpParser::parse(Buffer* buf) {
    while (true) {
        Step s = Step::kDone;
        switch (state_) {
            case kRequestLine:
                s = parseRequestLine(buf);
                if (s == Step::kDone) state_ = kHeaders;
                break;
            case kHeaders:
                s = parseHeaders(buf);
                if (s == Step::kDone) {
                    if (request_.getHeader("content-type").find("multipart/form-data") == 0) {
                        boundary_ = extractBoundary();
                        if (boundary_.empty()) {
                            setError("missing boundary in multipart/form-data");
                            return ParseResult::kError;
                        }
                        state_ = kMultipart;
                    } else if (contentLength_ > 0) {
                        state_ = kBody;
                    } else {
                        return ParseResult::kComplete;
                    }
                }
                break;
            case kBody:
                s = parseBody(buf);
                if (s == Step::kDone) return ParseResult::kComplete;
                break;
            case kMultipart:
                s = parseMultipart(buf);
                if (s == Step::kDone) return ParseResult::kComplete;
                break;
        }
        if (s == Step::kError) return ParseResult::kError;
        if (s == Step::kNeedMore) return ParseResult::kNeedMore;
        // s == Step::kDone 时继续循环，处理缓冲区中剩余数据
    }
}

std::string HttpParser::extractBoundary() const {
    const std::string& ct = request_.getHeader("content-type");
    const std::string key = "boundary=";
    size_t pos = ct.find(key);
    if (pos == std::string::npos) return std::string();
    std::string b = ct.substr(pos + key.size());
    b = unquote(trim(b));
    return b;
}

// ---------------- 请求行 ----------------

HttpParser::Step HttpParser::parseRequestLine(Buffer* buf) {
    const char* crlf = buf->findCRLF();
    if (crlf == nullptr) {
        if (buf->readableBytes() > kMaxRequestLine) {
            setError("request line too long");
            return Step::kError;
        }
        return Step::kNeedMore;
    }
    std::string line(buf->peek(), crlf);
    buf->retrieve(crlf - buf->peek() + 2);

    size_t sp1 = line.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        setError("malformed request line");
        return Step::kError;
    }
    request_.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    request_.version = line.substr(sp2 + 1);

    size_t q = target.find('?');
    if (q == std::string::npos) {
        request_.path = target;
    } else {
        request_.path = target.substr(0, q);
        request_.query = target.substr(q + 1);
    }
    return Step::kDone;
}

// ---------------- 请求头 ----------------

HttpParser::Step HttpParser::parseHeaders(Buffer* buf) {
    std::string_view view(buf->peek(), buf->readableBytes());
    size_t pos = view.find("\r\n\r\n");
    if (pos == std::string_view::npos) {
        if (buf->readableBytes() > kMaxHeaderBlock) {
            setError("header block too large");
            return Step::kError;
        }
        return Step::kNeedMore;
    }

    std::string block(view.substr(0, pos));
    buf->retrieve(pos + 4);

    // 逐行解析头部
    size_t start = 0;
    while (start < block.size()) {
        size_t eol = block.find("\r\n", start);
        if (eol == std::string::npos) eol = block.size();
        std::string line = block.substr(start, eol - start);
        if (!line.empty()) {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = toLower(trim(line.substr(0, colon)));
                std::string value = trim(line.substr(colon + 1));
                request_.headers[key] = value;
            }
        }
        start = eol + 2;
    }

    // content-length
    std::string cl = request_.getHeader("content-length");
    if (!cl.empty()) {
        try {
            contentLength_ = std::stoull(cl);
        } catch (...) {
            setError("invalid content-length");
            return Step::kError;
        }
    }
    return Step::kDone;
}

// ---------------- 普通请求体 ----------------

HttpParser::Step HttpParser::parseBody(Buffer* buf) {
    size_t need = contentLength_ - bodyRead_;
    size_t avail = buf->readableBytes();
    size_t take = std::min(need, avail);
    if (take > 0) {
        request_.body.append(buf->peek(), take);
        buf->retrieve(take);
        bodyRead_ += take;
    }
    if (bodyRead_ >= contentLength_) return Step::kDone;
    return Step::kNeedMore;
}

// ---------------- multipart/form-data 流式解析 ----------------

HttpParser::Step HttpParser::parseMultipart(Buffer* buf) {
    const std::string delimiter = "\r\n--" + boundary_;

    while (true) {
        switch (mpState_) {
            case kMpExpectBoundary: {
                // 首边界：body 以 "--boundary" 开头（前面无 \r\n）
                std::string first = "--" + boundary_;
                if (buf->readableBytes() < first.size() + 2) return Step::kNeedMore;
                std::string_view head(buf->peek(), first.size());
                if (head != std::string_view(first)) {
                    setError("multipart first boundary mismatch");
                    return Step::kError;
                }
                buf->retrieve(first.size());
                std::string_view tail(buf->peek(), 2);
                if (tail == "--") {
                    buf->retrieve(2);
                    mpState_ = kMpDone;
                    return Step::kDone; // 空表单
                }
                if (tail == "\r\n") {
                    buf->retrieve(2);
                    mpState_ = kMpPartHeaders;
                    mpHeaderBlock_.clear();
                    mpFirstBoundary_ = false;
                    break;
                }
                setError("malformed first boundary");
                return Step::kError;
            }

            case kMpPartHeaders: {
                // 不消费缓冲，直接在缓冲中寻找 \r\n\r\n
                std::string_view view(buf->peek(), buf->readableBytes());
                size_t pos = view.find("\r\n\r\n");
                if (pos == std::string_view::npos) {
                    if (buf->readableBytes() > kMaxHeaderBlock) {
                        setError("part header too large");
                        return Step::kError;
                    }
                    return Step::kNeedMore;
                }
                mpHeaderBlock_ = std::string(view.substr(0, pos));
                buf->retrieve(pos + 4);
                parseMultipartHeaders();
                mpState_ = kMpPartContent;
                if (isFilePart_ && !openTempFile()) return Step::kError;
                break;
            }

            case kMpPartContent: {
                std::string_view view(buf->peek(), buf->readableBytes());
                size_t pos = view.find(delimiter);
                if (pos == std::string_view::npos) {
                    // 未找到边界：消费到「可能的分界起点」之前，保留尾部
                    size_t safe = (buf->readableBytes() > delimiter.size() - 1)
                                      ? buf->readableBytes() - (delimiter.size() - 1)
                                      : 0;
                    if (safe == 0) return Step::kNeedMore;
                    if (isFilePart_) writeToFile(buf->peek(), safe);
                    else fieldValue_.append(buf->peek(), safe);
                    buf->retrieve(safe);
                    break;
                }
                // 找到边界，但需确保后面还有 2 字节可判断结尾符
                if (buf->readableBytes() - pos - delimiter.size() < 2) {
                    return Step::kNeedMore;
                }
                // 边界前的内容属于当前 part
                if (isFilePart_) {
                    writeToFile(buf->peek(), pos);
                    finalizeFilePart();
                } else {
                    fieldValue_.append(buf->peek(), pos);
                    if (fieldValue_.size() > kMaxFieldValue) {
                        setError("form field too large");
                        return Step::kError;
                    }
                    request_.form_fields[fieldName_] = fieldValue_;
                }
                buf->retrieve(pos + delimiter.size());

                std::string_view tail(buf->peek(), 2);
                if (tail == "--") {
                    buf->retrieve(2);
                    mpState_ = kMpDone;
                    return Step::kDone;
                }
                if (tail == "\r\n") {
                    buf->retrieve(2);
                    mpState_ = kMpPartHeaders;
                    mpHeaderBlock_.clear();
                    isFilePart_ = false;
                    fieldValue_.clear();
                    break;
                }
                setError("malformed multipart delimiter");
                return Step::kError;
            }

            case kMpDone:
                return Step::kDone;
        }
    }
}

void HttpParser::parseMultipartHeaders() {
    isFilePart_ = false;
    fieldName_.clear();

    size_t start = 0;
    std::string contentDisposition;
    std::string contentType;
    while (start <= mpHeaderBlock_.size()) {
        size_t eol = mpHeaderBlock_.find("\r\n", start);
        if (eol == std::string::npos) eol = mpHeaderBlock_.size();
        std::string line = mpHeaderBlock_.substr(start, eol - start);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = toLower(trim(line.substr(0, colon)));
            std::string value = trim(line.substr(colon + 1));
            if (key == "content-disposition") contentDisposition = value;
            else if (key == "content-type") contentType = value;
        }
        if (eol == mpHeaderBlock_.size()) break;
        start = eol + 2;
    }

    // 解析 Content-Disposition: form-data; name="x"; filename="y"
    size_t cur = contentDisposition.find(';');
    std::string fileName;
    while (cur != std::string::npos && cur < contentDisposition.size()) {
        size_t next = contentDisposition.find(';', cur + 1);
        std::string item = trim(contentDisposition.substr(
            cur + 1, (next == std::string::npos ? std::string::npos : next - cur - 1)));
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            std::string k = toLower(trim(item.substr(0, eq)));
            std::string v = unquote(item.substr(eq + 1));
            if (k == "name") fieldName_ = v;
            else if (k == "filename") fileName = v;
        }
        cur = next;
    }

    if (!fileName.empty()) {
        isFilePart_ = true;
        request_.file.filename = fileName;
        request_.file.field_name = fieldName_;
        request_.file.content_type = contentType;
    }
}

bool HttpParser::openTempFile() {
    tmpPath_ = "/tmp/transcode_upload_" + generateUuid() + ".part";
    tmpFd_ = ::open(tmpPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (tmpFd_ < 0) {
        setError("failed to create temp file");
        return false;
    }
    fileSize_ = 0;
    return true;
}

void HttpParser::writeToFile(const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(tmpFd_, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("write temp file failed: {}", strerror(errno));
            break;
        }
        off += static_cast<size_t>(n);
    }
    fileSize_ += off;
}

void HttpParser::finalizeFilePart() {
    if (tmpFd_ >= 0) {
        ::close(tmpFd_);
        tmpFd_ = -1;
    }
    request_.has_file = true;
    request_.file.field_name = fieldName_;
    request_.file.temp_path = tmpPath_;
    request_.file.size = fileSize_;
}

} // namespace transcode
