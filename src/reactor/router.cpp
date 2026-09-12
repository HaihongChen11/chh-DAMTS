#include "reactor/router.h"

#include <cctype>

namespace transcode {

std::string Router::key(const std::string& method, const std::string& path) {
    std::string m;
    m.reserve(method.size());
    for (char c : method) m.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return m + " " + path;
}

void Router::addRoute(const std::string& method, const std::string& path, Handler h) {
    routes_[key(method, path)] = std::move(h);
}

bool Router::route(const HttpRequest& req, HttpResponse* resp) const {
    auto it = routes_.find(key(req.method, req.path));
    if (it == routes_.end()) {
        *resp = HttpResponse::notFound("route not found: " + req.method + " " + req.path);
        return false;
    }
    it->second(req, resp);
    return true;
}

} // namespace transcode
