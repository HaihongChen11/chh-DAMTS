#pragma once

#include <memory>
#include <string>

#include "common/noncopyable.h"

namespace prometheus {
class Registry;
class Counter;
class Gauge;
}

namespace transcode {

// 业务指标埋点（prometheus-cpp）：QPS、任务提交/成功/失败计数、在途任务数。
// 单例，供 api_server 与 worker 共用；由 api_server 的 /metrics 端点暴露。
class Metrics : Noncopyable {
public:
    static Metrics& instance();

    void init();
    std::string serialize(); // Prometheus 文本格式

    void incQps();
    void incTaskSubmitted();
    void incTaskSuccess();
    void incTaskFailed();
    void incInFlight();
    void decInFlight();

private:
    Metrics() = default;

    std::shared_ptr<prometheus::Registry> registry_;
    prometheus::Counter* qps_ = nullptr;
    prometheus::Counter* taskSubmitted_ = nullptr;
    prometheus::Counter* taskSuccess_ = nullptr;
    prometheus::Counter* taskFailed_ = nullptr;
    prometheus::Gauge* inFlight_ = nullptr;
};

} // namespace transcode
