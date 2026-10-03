#include <csignal>
#include <memory>
#include <thread>
#include <vector>

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
transcode::EventLoop* g_acceptLoop = nullptr;
transcode::HttpServer* g_server = nullptr;
std::vector<transcode::EventLoop*> g_subLoops;

void signalHandler(int /*sig*/) {
    if (g_server) g_server->stopAccepting();
    if (g_acceptLoop) g_acceptLoop->quit();
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

    transcode::EventLoop acceptLoop;
    g_acceptLoop = &acceptLoop;

    constexpr size_t kReactorThreads = 8;
    std::vector<std::unique_ptr<transcode::EventLoop>> subLoopOwners;
    subLoopOwners.reserve(kReactorThreads);
    for (size_t i = 0; i < kReactorThreads; ++i) {
        subLoopOwners.push_back(std::make_unique<transcode::EventLoop>());
        g_subLoops.push_back(subLoopOwners.back().get());
    }

    std::vector<std::thread> subThreads;
    subThreads.reserve(kReactorThreads);
    for (auto& sub : subLoopOwners) {
        subThreads.emplace_back([loop = sub.get()]() { loop->loop(); });
    }

    transcode::Router router;
    transcode::registerRoutes(&router, &svc);

    transcode::HttpServer server(&acceptLoop, g_subLoops,
                                 cfg.server().host, cfg.server().port);
    g_server = &server;
    server.setConnectionTimeoutMs(cfg.server().connection_timeout_ms);
    server.setHttpCallback(transcode::makeHttpCallback(&router, &svc, &server));
    server.start();

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    acceptLoop.loop();

    for (auto* sub : g_subLoops) sub->quit();
    for (auto& t : subThreads) {
        if (t.joinable()) t.join();
    }

    svc.shutdown();
    Aws::ShutdownAPI(awsOptions);
    return 0;
}
