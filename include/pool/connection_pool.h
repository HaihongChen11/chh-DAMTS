#pragma once

#include <condition_variable>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "common/noncopyable.h"

namespace transcode {

// 通用连接池模板：适配 MySQL、Redis、RabbitMQ、MinIO 等客户端连接。
//
// 设计要点：
//  1. Factory 负责创建连接，Validator 负责校验复用连接是否仍然有效；
//  2. acquire() 返回 shared_ptr<T>，其自定义 deleter 会在引用计数归零时把连接
//     归还到空闲队列（RAII 自动归还，业务代码无需手动 release）；
//  3. 控制最大连接数，空闲池空且未达上限才新建，否则阻塞等待归还。
//
// 注意：连接池对象必须比所有借出的连接存活更久（正常作为服务级单例使用）。
template <typename T>
class ConnectionPool : Noncopyable {
public:
    using Ptr = std::shared_ptr<T>;
    using Factory = std::function<std::unique_ptr<T>()>;
    using Validator = std::function<bool(T*)>;

    ConnectionPool(Factory factory, size_t max_size, Validator validator = nullptr)
        : factory_(std::move(factory)),
          validator_(std::move(validator)),
          maxSize_(max_size == 0 ? 4 : max_size) {}

    ~ConnectionPool() {
        std::lock_guard<std::mutex> lk(mutex_);
        for (T* c : idle_) delete c;
        idle_.clear();
    }

    // 借出连接（自动归还）
    Ptr acquire() {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this]() { return !idle_.empty() || total_ < maxSize_; });
        return acquireLocked(lk);
    }

    // 带超时等待的连接获取，超时返回 nullptr
    Ptr acquire(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mutex_);
        if (!cv_.wait_for(lk, timeout,
                          [this]() { return !idle_.empty() || total_ < maxSize_; })) {
            return nullptr;
        }
        return acquireLocked(lk);
    }

    size_t maxSize() const { return maxSize_; }

private:
    Ptr acquireLocked(std::unique_lock<std::mutex>& lk) {
        T* raw = nullptr;
        for (;;) {
            if (!idle_.empty()) {//空闲队列非空
                raw = idle_.front();
                idle_.pop_front();
                if (validator_ && !validator_(raw)) {//
                    delete raw;
                    --total_;
                    continue; // 失效连接丢弃，尝试下一个
                }
                break;
            }
            // 新建连接
            ++total_;
            lk.unlock();
            std::unique_ptr<T> created = factory_();
            raw = created.release();
            lk.lock();
            if (raw == nullptr) {
                --total_;
                cv_.notify_one();
                return nullptr;
            }
            break;
        }

        // 自定义 deleter（第二个参数lambda）：引用计数归零时归还，而不是销毁。复用连接，减少创建/销毁开销
        return Ptr(raw, [this](T* p) {
            std::lock_guard<std::mutex> l(mutex_);
            idle_.push_back(p);
            cv_.notify_one();
        });
    }

    Factory factory_;
    Validator validator_;
    size_t maxSize_;
    size_t total_ = 0;
    std::deque<T*> idle_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace transcode
