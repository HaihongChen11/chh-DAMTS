#include <csignal>

#include <aws/core/Aws.h>

#include "api_server/http_handler.h"
#include "common/config.h"
#include "common/logger.h"
#include "metrics/metrics.h"
#include "reactor/event_loop.h"
#include "reactor/http_server.h"
#include "reactor/router.h"
#include "service/service_registry.h"

namespace {
transcode::EventLoop* g_loop = nullptr;

void signalHandler(int /*sig*/) {
    // quit() 仅置原子标志 + 写 eventfd 唤醒，异步信号安全
    if (g_loop) g_loop->quit();
}
} // namespace

int main(int argc, char* argv[]) {
    std::string configPath = (argc > 1) ? argv[1] : "config.json";
    transcode::initLogger("info");
    transcode::Metrics::instance().init();

    Aws::SDKOptions awsOptions;
    Aws::InitAPI(awsOptions);

    transcode::Config cfg;
    if (!cfg.load(configPath)) {
        Aws::ShutdownAPI(awsOptions);
        return 1;
    }

    transcode::ServiceRegistry svc;
    if (!svc.init(cfg)) {
        Aws::ShutdownAPI(awsOptions);
        return 1;
    }

    transcode::EventLoop loop;
    g_loop = &loop;

    transcode::Router router;
    transcode::registerRoutes(&router, &svc);

    transcode::HttpServer server(&loop, cfg.server().host, cfg.server().port);
    server.setConnectionTimeoutMs(cfg.server().connection_timeout_ms);
    server.setHttpCallback(transcode::makeHttpCallback(&router, &svc));
    server.start();

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    loop.loop();

    svc.shutdown();
    Aws::ShutdownAPI(awsOptions);
    return 0;
}
