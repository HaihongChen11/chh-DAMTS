#include "metrics/metrics.h"

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <prometheus/text_serializer.h>

namespace transcode {

Metrics& Metrics::instance() {
    static Metrics inst;
    return inst;
}

void Metrics::init() {
    using namespace prometheus;
    registry_ = std::make_shared<Registry>();

    qps_ = &BuildCounter()
                .Name("http_requests_total")
                .Help("Total number of HTTP requests")
                .Register(*registry_)
                .Add({});

    taskSubmitted_ = &BuildCounter()
                          .Name("transcode_tasks_submitted_total")
                          .Help("Total submitted transcode tasks")
                          .Register(*registry_)
                          .Add({});

    taskSuccess_ = &BuildCounter()
                        .Name("transcode_tasks_success_total")
                        .Help("Total successful transcode tasks")
                        .Register(*registry_)
                        .Add({});

    taskFailed_ = &BuildCounter()
                       .Name("transcode_tasks_failed_total")
                       .Help("Total failed transcode tasks")
                       .Register(*registry_)
                       .Add({});

    inFlight_ = &BuildGauge()
                     .Name("transcode_tasks_inflight")
                     .Help("Number of in-flight transcode tasks")
                     .Register(*registry_)
                     .Add({});
}

std::string Metrics::serialize() {
    if (!registry_) return std::string();
    prometheus::TextSerializer serializer;
    return serializer.Serialize(registry_->Collect());
}

void Metrics::incQps() {
    if (qps_) qps_->Increment();
}
void Metrics::incTaskSubmitted() {
    if (taskSubmitted_) taskSubmitted_->Increment();
}
void Metrics::incTaskSuccess() {
    if (taskSuccess_) taskSuccess_->Increment();
}
void Metrics::incTaskFailed() {
    if (taskFailed_) taskFailed_->Increment();
}
void Metrics::incInFlight() {
    if (inFlight_) inFlight_->Increment();
}
void Metrics::decInFlight() {
    if (inFlight_) inFlight_->Decrement();
}

} // namespace transcode
