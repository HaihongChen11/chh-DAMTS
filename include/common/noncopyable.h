#pragma once

namespace transcode {

// 禁止拷贝的基类，被多数资源持有类继承（fd、连接、线程池等）
class Noncopyable {
public:
    Noncopyable() = default;
    ~Noncopyable() = default;

    Noncopyable(const Noncopyable&) = delete;
    Noncopyable& operator=(const Noncopyable&) = delete;
};

} // namespace transcode
