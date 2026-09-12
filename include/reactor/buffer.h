#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <sys/types.h>

namespace transcode {

// 网络字节缓冲区：内部使用 vector + 读写双指针，避免频繁扩容。
// 支持从 fd 读取（readv 一次读到栈 + 缓冲）与向 fd 写出。
class Buffer {
public:
    static constexpr size_t kInitialSize = 1024;
    static constexpr size_t kCheapPrepend = 8;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {}

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    const char* peek() const { return begin() + readerIndex_; }
    char* beginWrite() { return begin() + writerIndex_; }
    const char* beginWrite() const { return begin() + writerIndex_; }

    void retrieve(size_t len) {
        assert(len <= readableBytes());
        if (len < readableBytes()) {
            readerIndex_ += len;
        } else {
            retrieveAll();
        }
    }

    void retrieveAll() {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    std::string retrieveAsString(size_t len) {
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }

    void append(const char* data, size_t len) {
        ensureWritableBytes(len);
        std::copy(data, data + len, beginWrite());
        hasWritten(len);
    }
    void append(const std::string& s) { append(s.data(), s.size()); }

    void ensureWritableBytes(size_t len) {
        if (writableBytes() < len) makeSpace(len);
    }
    void hasWritten(size_t len) { writerIndex_ += len; }

    // 查找 CRLF，用于 HTTP 逐行解析
    const char* findCRLF() const {
        const char* crlf = std::search(peek(), beginWrite(), kCRLF, kCRLF + 2);
        return crlf == beginWrite() ? nullptr : crlf;
    }
    const char* findCRLF(const char* start) const {
        const char* crlf = std::search(start, beginWrite(), kCRLF, kCRLF + 2);
        return crlf == beginWrite() ? nullptr : crlf;
    }

    // 从 fd 读取数据（readv：buffer + 栈缓冲，减少系统调用）
    ssize_t readFd(int fd, int* savedErrno);
    // 向 fd 写出可读数据
    ssize_t writeFd(int fd, int* savedErrno);

    void swap(Buffer& rhs) {
        buffer_.swap(rhs.buffer_);
        std::swap(readerIndex_, rhs.readerIndex_);
        std::swap(writerIndex_, rhs.writerIndex_);
    }

private:
    char* begin() { return buffer_.data(); }
    const char* begin() const { return buffer_.data(); }

    void makeSpace(size_t len) {
        if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
            // 空间不足，扩容
            buffer_.resize(writerIndex_ + len);
        } else {
            // 将未读数据搬到最前面，腾出尾部空间
            size_t readable = readableBytes();
            std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;

    static const char kCRLF[];
};

} // namespace transcode
