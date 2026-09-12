#pragma once

#include <memory>
#include <string>

#include "common/config.h"
#include "common/noncopyable.h"

namespace elasticlient { class Client; }
namespace transcode { struct VideoMeta; }

namespace transcode {

// Elasticsearch 客户端封装：转码完成后同步视频元数据，提供全文检索。
class EsService : Noncopyable {
public:
    explicit EsService(const Config::Elasticsearch& cfg);

    // 索引视频元数据（task_id 作为文档 id，天然幂等）
    bool indexMeta(const VideoMeta& meta);
    // 全文检索，返回命中结果的 JSON 字符串
    std::string search(const std::string& query);

private:
    Config::Elasticsearch cfg_;
    std::shared_ptr<elasticlient::Client> client_;
};

} // namespace transcode
