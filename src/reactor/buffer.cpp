#include "reactor/buffer.h"

#include <cerrno>
#include <sys/uio.h>
#include <unistd.h>

namespace transcode {

const char Buffer::kCRLF[] = "\r\n";

ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extrabuf[65536];
    struct iovec vec[2];
    const size_t writable = writableBytes();

    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        writerIndex_ += n;
    } else {
        // 栈缓冲也读满了
        writerIndex_ = buffer_.size();
        append(extrabuf, n - writable);
    }
    return n;
}

ssize_t Buffer::writeFd(int fd, int* savedErrno) {
    const size_t readable = readableBytes();
    if (readable == 0) return 0;
    const ssize_t n = ::write(fd, peek(), readable);
    if (n < 0) {
        *savedErrno = errno;
    } else if (n > 0) {
        retrieve(n);
    }
    return n;
}

} // namespace transcode
