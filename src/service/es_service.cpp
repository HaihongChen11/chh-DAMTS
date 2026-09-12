#include "service/es_service.h"

#include <vector>

#include <cpr/cpr.h>
#include <elasticlient/client.h>
#include <nlohmann/json.hpp>

#include "common/logger.h"
#include "service/types.h"

namespace transcode {

EsService::EsService(const Config::Elasticsearch& cfg) : cfg_(cfg) {
    client_ = std::make_shared<elasticlient::Client>(cfg.hosts, 600);
}

bool EsService::indexMeta(const VideoMeta& meta) {
    try {
        // elasticlient 仍要求 docType 参数；ES7 下使用固定 "_doc" 作为类型段。
        client_->index(cfg_.index, "_doc", meta.task_id, meta.toJson().dump());
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("es index meta failed: {}", e.what());
        return false;
    }
}

std::string EsService::search(const std::string& query) {
    nlohmann::json body;
    body["query"]["multi_match"]["query"] = query;
    body["query"]["multi_match"]["fields"] = {"video_name", "tag"};
    try {
        cpr::Response resp = client_->search(cfg_.index, "_doc", body.dump());
        return resp.text;
    } catch (const std::exception& e) {
        LOG_ERROR("es search failed: {}", e.what());
        return "{}";
    }
}

} // namespace transcode
