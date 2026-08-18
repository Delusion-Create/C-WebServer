#include "Router.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "RateLimiter.h"
#include <utility>

void Router::add(const std::string& method, const std::string& pattern, Handler handler, bool rateLimited)
{
    Route route;
    route.method = method;
    route.prefix = false;
    route.rateLimited = rateLimited;
    route.handler = std::move(handler);

    // 解析 pattern: "/a/b/c" -> ["a","b","c"]; 末尾 "/*" 表示前缀路由
    std::string p = pattern;
    if (p.size() > 2 && p.compare(p.size() - 2, 2, "/*") == 0) {
        route.prefix = true;
        p = p.substr(0, p.size() - 2);
    }

    size_t pos = 0;
    while (pos < p.size()) {
        if (p[pos] != '/') { ++pos; continue; }
        size_t end = p.find('/', pos + 1);
        if (end == std::string::npos) end = p.size();
        if (end > pos + 1) {
            route.segments.push_back(p.substr(pos + 1, end - pos - 1));
        }
        pos = end;
    }
    _routes.push_back(std::move(route));
}

bool Router::matchSegments(const Route& route, const std::vector<std::string>& pathSegs,
                           std::vector<std::string>& params) const
{
    params.clear();
    if (route.prefix) {
        // 前缀匹配: 前半段逐段匹配, 剩余部分拼接为最后一个参数
        if (pathSegs.size() < route.segments.size()) return false;
        for (size_t i = 0; i < route.segments.size(); ++i) {
            if (route.segments[i][0] == ':') {
                params.push_back(pathSegs[i]);
            } else if (route.segments[i] != pathSegs[i]) {
                return false;
            }
        }
        std::string rest;
        for (size_t i = route.segments.size(); i < pathSegs.size(); ++i) {
            if (i > route.segments.size()) rest += "/";
            rest += pathSegs[i];
        }
        params.push_back(rest);
        return true;
    }

    if (pathSegs.size() != route.segments.size()) return false;
    for (size_t i = 0; i < route.segments.size(); ++i) {
        if (route.segments[i][0] == ':') {
            params.push_back(pathSegs[i]);
        } else if (route.segments[i] != pathSegs[i]) {
            return false;
        }
    }
    return true;
}

bool Router::dispatch(const HttpRequest& req, HttpResponse& resp, const std::string& clientIp) const
{
    // 按 '/' 拆分请求路径
    const std::string& path = req.getPath();
    std::vector<std::string> pathSegs;
    size_t pos = 0;
    while (pos < path.size()) {
        if (path[pos] != '/') { ++pos; continue; }
        size_t end = path.find('/', pos + 1);
        if (end == std::string::npos) end = path.size();
        if (end > pos + 1) {
            pathSegs.push_back(path.substr(pos + 1, end - pos - 1));
        }
        pos = end;
    }

    const std::string& method = req.getMethod();
    bool pathMatched = false; // 路径存在但方法不匹配 -> 405

    for (const Route& route : _routes) {
        std::vector<std::string> params;
        if (!matchSegments(route, pathSegs, params)) continue;

        if (route.method != method) {
            pathMatched = true; // 继续找同路径下是否有匹配方法的路由
            continue;
        }

        // 方法匹配: 先做限流判断
        if (route.rateLimited && !RateLimiter::instance().allow(clientIp)) {
            resp.setStatus(429);
            resp.setBody("429 Too Many Requests: rate limit exceeded.");
            return true;
        }

        route.handler(req, params, resp);
        return true;
    }

    if (pathMatched) {
        resp.setStatus(405);
        resp.setBody("405 Method Not Allowed: The requested method is not supported.");
        return true;
    }
    return false;
}
